#include "global.h"
#include "coop.h"
#include "coop_battle.h"
#include "battle.h"
#include "battle_setup.h"
#include "battle_main.h"
#include "link.h"
#include "net_link.h"
#include "task.h"
#include "palette.h"
#include "overworld.h"
#include "field_screen_effect.h"
#include "field_weather.h"
#include "main.h"
#include "sound.h"
#include "script.h"
#include "constants/songs.h"
#include "constants/battle.h"
#include "data.h"
#include "pokemon.h"
#include "battle_util.h"

// ---------------------------------------------------------------------------
// Co-op battles: both players against the same trainers, at once.
//
// This is a REAL link battle, not a simulation of one. The two consoles run the
// same battle in lockstep over the same transport the overworld uses, with each
// player controlling their own Pokemon and the AI opponents driven by whichever
// console the handshake picks as master.
//
// The shape is the Battle Tower's link multi battle, which is the only
// configuration in the game that means "two linked humans on one side": flags
// DOUBLE | LINK | TRAINER | MULTI | BATTLE_TOWER, handshake
// CB2_HandleStartMultiPartnerBattle. BATTLE_TOWER is doing real work there and
// is not cosmetic -- it is what collapses the block-exchange gates from four
// players to two in five separate places, so dropping it means replacing all
// five rather than deleting a flag.
//
// Getting in and out is the delicate part. The battle wants the link CLOSED
// when it starts, because the first thing it does is open one and run its own
// player exchange -- so the entry below tears the co-op session's link down and
// waits for it to go, exactly as the cable club does before a link battle. The
// co-op session layer is suspended across the whole thing and rebuilt
// afterwards.
// ---------------------------------------------------------------------------

#define tState   data[0]
#define tTimer   data[1]

// How long to wait for the link to close before giving up.
//
// It should take a handful of frames. If it has not happened in five seconds
// the peer is not answering, and the right thing is to put the player back in
// the overworld rather than leave them on a black screen for ever.
#define CLOSE_TIMEOUT_FRAMES 300

static EWRAM_DATA bool8 sCoopBattleActive = FALSE;

bool8 Coop_IsBattleActive(void)
{
    return sCoopBattleActive;
}

// Back to the field once the battle is over, with the script that started it
// carrying on from its waitstate.
static void CB2_ReturnFromCoopBattle(void)
{
    sCoopBattleActive = FALSE;

    // Rebuild the co-op session. The battle left the link closed, which is the
    // state the session machine starts from anyway.
    Coop_ResumeAfterBattle();

    SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
}

static void Task_CoopBattleStart(u8 taskId)
{
    struct Task *task = &gTasks[taskId];

    switch (task->tState)
    {
    case 0:
        FadeScreen(FADE_TO_BLACK, 0);
        gLinkType = LINKTYPE_BATTLE;

        // Record who is here, before the link goes down.
        //
        // The battle reopens the link and then checks the players that come
        // back against a list saved earlier -- a list the cable club fills in
        // its lobby, which a co-op battle never visits. Left unsaved the count
        // is zero, the check compares two players against none, and
        // Task_WaitForLinkPlayerConnection throws up "Communication error"
        // before the battle has done anything at all. This is the lobby's job,
        // done here because this is our lobby.
        SaveLinkPlayers(NET_MAX_PLAYERS);

        ClearLinkCallback_2();
        // Before the link is touched: the session layer must not react to the
        // teardown below by trying to rebuild what the battle is dismantling.
        Coop_SuspendForBattle();
        task->tState++;
        break;

    case 1:
        if (!gPaletteFade.active)
            task->tState++;
        break;

    case 2:
        // The cable club waits here too. The peer needs long enough to reach
        // its own teardown, and both sides are running the same script, so a
        // fixed pause is all the agreement that is needed.
        if (++task->tTimer > 20)
        {
            task->tTimer = 0;
            task->tState++;
        }
        break;

    case 3:
        SetCloseLinkCallback();
        task->tState++;
        break;

    case 4:
        if (!gReceivedRemoteLinkPlayers)
        {
            task->tState++;
        }
        else if (++task->tTimer > CLOSE_TIMEOUT_FRAMES)
        {
            // The link would not close. Put the player back rather than leaving
            // them staring at black: the battle cannot start without both
            // consoles, and the session layer can rebuild from here.
            Coop_ResumeAfterBattle();
            sCoopBattleActive = FALSE;
            SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
            DestroyTask(taskId);
        }
        break;

    case 5:
        PlayMapChosenOrBattleBGM(MUS_VS_GYM_LEADER);

        gBattleTypeFlags = BATTLE_TYPE_BATTLE_TOWER
                         | BATTLE_TYPE_DOUBLE
                         | BATTLE_TYPE_LINK
                         | BATTLE_TYPE_TRAINER
                         | BATTLE_TYPE_MULTI;

        // Build the opponents' parties ourselves.
        //
        // CB2_InitBattleInternal skips its own CreateNPCTrainerParty whenever
        // BATTLE_TYPE_LINK is set (src/battle_main.c:597), because in a cable
        // link battle the "opponents" are the other humans and there is nothing
        // to generate. Ours are real trainers, so if we do not fill these the
        // battle starts against two empty parties.
        //
        // Both consoles build them, and they will not match -- party generation
        // draws personality and IVs from the RNG. That is expected and
        // harmless: the handshake has the master broadcast both parties and the
        // other console overwrite its copies, so the master's are the ones
        // fought. Building on both anyway means neither console depends on
        // being the one that won the master coin toss.
        //
        // halfTeam is where "3 each" and "6 each" are actually decided; see
        // AreMultiPartiesFullTeams.
        {
            bool32 halfTeam = !AreMultiPartiesFullTeams();

            CreateNPCTrainerPartyFromTrainer(
                &gParties[B_TRAINER_1][0],
                GetTrainerStructFromId(TRAINER_BATTLE_PARAM.opponentA),
                halfTeam, gBattleTypeFlags);
            CreateNPCTrainerPartyFromTrainer(
                &gParties[B_TRAINER_3][0],
                GetTrainerStructFromId(TRAINER_BATTLE_PARAM.opponentB),
                halfTeam, gBattleTypeFlags);

            SetWildMonHeldItem();
            CalculateEnemyPartyCount();
        }

        CleanupOverworldWindowsAndTilemaps();
        gMain.savedCallback = CB2_ReturnFromCoopBattle;
        SetMainCallback2(CB2_InitBattle);
        DestroyTask(taskId);
        break;
    }
}

#undef tState
#undef tTimer

void Coop_StartBattle(u16 opponentA, u16 opponentB)
{
    TRAINER_BATTLE_PARAM.opponentA = opponentA;
    TRAINER_BATTLE_PARAM.opponentB = opponentB;

    sCoopBattleActive = TRUE;
    CreateTask(Task_CoopBattleStart, 0);
}
