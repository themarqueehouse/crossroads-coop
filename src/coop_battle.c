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

// Whether the two opposing trainer slots share ONE trainer's team.
//
// A gym leader has no partner, and pairing them with a real gym trainer costs
// the gym a fight. So the second slot is a filler who brings nothing of their
// own: the leader's team is dealt across the two of them. The opposition is
// exactly the Pokemon the leader always had, and it is a double battle because
// there are two trainers standing there -- 2v1 in substance, 2v2 in shape.
static EWRAM_DATA bool8 sSplitTeam = FALSE;

// What to run once the battle is over, if anything already wanted to.
//
// A gym leader's battle is started by the game's own trainerbattle machinery,
// which sets CB2_EndTrainerBattle to mark the trainer defeated, handle losing
// and let the script carry on to the badge. Co-op runs AFTER that is decided
// rather than instead of it, so none of it has to be reimplemented here.
static EWRAM_DATA MainCallback sChainedCallback = NULL;

// A battle the script has asked to be fought co-op, set just before the
// ordinary trainerbattle command that starts it.
//
// Marking the next battle rather than replacing the command is what keeps the
// intro text, the defeat text, the trainer flag, the jump to the badge script
// and the whiteout all working: they belong to trainerbattle, and trainerbattle
// still runs.
static EWRAM_DATA u16 sNextPartner = 0;
static EWRAM_DATA bool8 sNextSplit = FALSE;
static EWRAM_DATA bool8 sNextMarked = FALSE;

bool8 Coop_IsBattleActive(void)
{
    return sCoopBattleActive;
}

bool8 Coop_BattleSplitsTeam(void)
{
    return sCoopBattleActive && sSplitTeam;
}

void Coop_BuildSplitOpponents(void)
{
    u8 count, keep, i;

    // The whole team into the first slot. halfTeam is FALSE deliberately: we
    // want everything the trainer has before deciding how to share it out,
    // and the frontier bits are off for the reason CreateNPCTrainerParty
    // explains at length.
    CreateNPCTrainerPartyFromTrainer(&gParties[B_TRAINER_1][0],
                                     GetTrainerStructFromId(TRAINER_BATTLE_PARAM.opponentA),
                                     FALSE,
                                     BATTLE_TYPE_TRAINER | BATTLE_TYPE_DOUBLE);

    count = CalculatePartyCount(B_TRAINER_1);
    // The odd one stays with the real trainer, so a five-Pokemon leader keeps
    // three and the filler takes two rather than the other way round.
    keep = (count + 1) / 2;

    ZeroPartyMons(gParties[B_TRAINER_3]);

    for (i = keep; i < count; i++)
    {
        gParties[B_TRAINER_3][i - keep] = gParties[B_TRAINER_1][i];
        ZeroMonData(&gParties[B_TRAINER_1][i]);
    }
}

// Back to the field once the battle is over, with the script that started it
// carrying on from its waitstate.
static void CB2_ReturnFromCoopBattle(void)
{
    MainCallback next = sChainedCallback;

    sCoopBattleActive = FALSE;
    sSplitTeam = FALSE;
    sChainedCallback = NULL;

    // Rebuild the co-op session. The battle left the link closed, which is the
    // state the session machine starts from anyway.
    Coop_ResumeAfterBattle();

    SetMainCallback2(next != NULL ? next : CB2_ReturnToFieldContinueScriptPlayMapMusic);
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

        CleanupOverworldWindowsAndTilemaps();
        gMain.savedCallback = CB2_ReturnFromCoopBattle;
        SetMainCallback2(CB2_InitBattle);
        DestroyTask(taskId);
        break;
    }
}

#undef tState
#undef tTimer

void Coop_StartBattle(u16 opponentA, u16 opponentB, bool8 splitTeam)
{
    TRAINER_BATTLE_PARAM.opponentA = opponentA;
    TRAINER_BATTLE_PARAM.opponentB = opponentB;

    sCoopBattleActive = TRUE;
    sSplitTeam = splitTeam;
    sChainedCallback = gMain.savedCallback;
    // Breadcrumb triangulation: this function certainly runs, so if the rig
    // reads 0 here the problem is the reading, not the running.
    gCoopDbgReached = 7;
    CreateTask(Task_CoopBattleStart, 0);
}

void Coop_MarkNextBattle(u16 partnerTrainer, bool8 split)
{
    sNextPartner = partnerTrainer;
    sNextSplit = split;
    sNextMarked = TRUE;
}

bool8 Coop_TakeOverTrainerBattle(void)
{
    if (!sNextMarked)
        return FALSE;

    // One battle per mark, whatever happens next. Leaving it set would make
    // the following unrelated trainer a co-op battle too.
    sNextMarked = FALSE;

    // No partner, no co-op battle -- the caller goes on to start the ordinary
    // one it was always going to, so the gym still works single-player.
    if (!Coop_PartnerIsHere())
        return FALSE;

    Coop_StartBattle(TRAINER_BATTLE_PARAM.opponentA, sNextPartner, sNextSplit);
    return TRUE;
}
