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
#include "load_save.h"
#include "party_menu.h"
#include "script_pokemon_util.h"

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

#define tState    data[0]
#define tTimer    data[1]
#define tClosedAt data[2]
#define tEvicted  data[3]

// How long to wait for the link to close before giving up.
//
// It should take a handful of frames. If it has not happened in five seconds
// the peer is not answering, and the right thing is to put the player back in
// the overworld rather than leave them on a black screen for ever.
#define CLOSE_TIMEOUT_FRAMES 300

// How long to sit still after our own link has closed, before opening the
// battle's. Long enough for a partner a frame or two behind to finish theirs.
#define CLOSE_SETTLE_FRAMES 30

static EWRAM_DATA bool8 sCoopBattleActive = FALSE;

// The two players against each other, rather than side by side.
//
// Deliberately NOT sCoopBattleActive. That flag means "a link battle whose
// opponents are AI trainers", and everything keyed on it -- generating the
// opposing parties, naming them from the trainer table, awarding experience --
// is wrong here. A battle between the two players is an ordinary link battle
// and every piece of vanilla link behaviour is already correct for it. This
// flag exists only for the way back out.
static EWRAM_DATA bool8 sPvpActive = FALSE;

// Whether the two opposing trainer slots share ONE trainer's team.
//
// A gym leader has no partner, and pairing them with a real gym trainer costs
// the gym a fight. So the second slot is a filler who brings nothing of their
// own: the leader's team is dealt across the two of them. The opposition is
// exactly the Pokemon the leader always had, and it is a double battle because
// there are two trainers standing there -- 2v1 in substance, 2v2 in shape.
static EWRAM_DATA bool8 sSplitTeam = FALSE;
// Whether this battle actually took three Pokemon away, and so owes them back.
static EWRAM_DATA bool8 sReducedParty = FALSE;

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

bool8 Coop_IsPvpActive(void)
{
    return sPvpActive;
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

    // The other three back, before anything else looks at the party -- but only
    // if they were ever taken away.
    if (sReducedParty)
        LoadPlayerParty();

    sCoopBattleActive = FALSE;
    sSplitTeam = FALSE;
    sReducedParty = FALSE;
    sChainedCallback = NULL;

    // Rebuild the co-op session. The battle left the link closed, which is the
    // state the session machine starts from anyway.
    Coop_ResumeAfterBattle();

    SetMainCallback2(next != NULL ? next : CB2_ReturnToFieldContinueScriptPlayMapMusic);
}

// Back to the field after the two players have fought each other.
//
// The party comes back exactly as it went in. A battle between friends costs
// nothing -- nobody faints for real, nobody loses money, nobody walks to a
// Center afterwards -- which is the same bargain the cable club strikes
// (CB2_ReturnFromCableClubBattle restores the party too) and the reason a link
// battle can be fought standing in the middle of a route.
//
// No win/loss record is kept. The trainer card's link record is keyed on the
// trainer cards the cable club's lobby exchanges, which this never visits, so
// writing one would file the result against a blank name.
static void CB2_ReturnFromPvpBattle(void)
{
    LoadPlayerParty();

    gBattleTypeFlags &= ~BATTLE_TYPE_LINK_IN_BATTLE;
    Overworld_ResetMapMusic();

    sPvpActive = FALSE;
    Coop_ResumeAfterBattle();

    SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
}

// How far the entry got, and whether it gave up. Read by the test rig by
// symbol address.
//
// Everything this task does happens behind a fade to black, so from outside
// there is nothing to see but a black screen on one console and an overworld
// on the other -- which is the same picture whether the link would not close,
// the battle never started, or it started and ended instantly.
EWRAM_DATA u8 gCoopDbgBattleStep = 0;
EWRAM_DATA u8 gCoopDbgBattleBail = 0;
EWRAM_DATA u8 gCoopDbgBattlePlayers = 0;
EWRAM_DATA u8 gCoopDbgBattleCb = 0;
EWRAM_DATA u8 gCoopDbgBattleQueue = 0;
EWRAM_DATA u8 gCoopDbgCloseTrace[24] = {0};
EWRAM_DATA u8 gCoopDbgCloseTraceLen = 0;

static void Task_CoopBattleStart(u8 taskId)
{
    struct Task *task = &gTasks[taskId];

    gCoopDbgBattleStep = task->tState;
    gCoopDbgBattlePlayers = gReceivedRemoteLinkPlayers;
    // Which stage of the close this console reached. SetCloseLinkCallback
    // quietly does NOTHING when a link callback is already installed, and
    // LinkCB_ReadyCloseLink only arms the wait once the receive queue is
    // empty, so "the link would not close" has three quite different causes
    // and they are indistinguishable from the black screen they all produce.
    gCoopDbgBattleCb = Coop_LinkCloseStage();
    gCoopDbgBattleQueue = GetLinkRecvQueueLength();

    // Every change in the close's state, in order, because the interesting
    // ones last a frame and a sampling test rig walks straight past them.
    // Each byte is the close stage in the low nibble and whether the link was
    // up in the high one.
    {
        u8 mark = gCoopDbgBattleCb
                | ((gLinkStatus & LINK_STAT_CONN_ESTABLISHED) ? 0x10 : 0)
                | (gReceivedRemoteLinkPlayers ? 0x20 : 0);
        if (gCoopDbgCloseTraceLen == 0
            || (gCoopDbgCloseTraceLen < ARRAY_COUNT(gCoopDbgCloseTrace)
                && gCoopDbgCloseTrace[gCoopDbgCloseTraceLen - 1] != mark))
        {
            gCoopDbgCloseTrace[gCoopDbgCloseTraceLen++] = mark;
        }
    }

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
        // Remember how many closes have finished, so the next state can tell
        // ours apart from any that came before.
        task->tClosedAt = gCoopLinkClosedCount;
        SetCloseLinkCallback();
        task->tState++;
        break;

    case 4:
        // Our own close finishing is what we are waiting for -- not
        // gReceivedRemoteLinkPlayers falling.
        //
        // That flag was the original test and it is unreliable here for a
        // reason that only shows up with two consoles doing this at once. The
        // partner is a frame or two ahead; the moment their close completes
        // they open a NEW link for the battle, and its player exchange sets
        // the flag straight back to 1 -- often within the same frame this
        // state was going to look at it. The console a frame behind then waits
        // for a moment that has already passed, times out, and walks back into
        // the overworld while its partner sits in a battle waiting for a
        // player who is never coming. That is exactly what the first two
        // players to try battling each other saw: one black screen, one field.
        //
        // The counter only goes up, so it cannot be missed. The flag stays as
        // a second way through for the case where there was nothing to close.
        if (gCoopLinkClosedCount != task->tClosedAt || !gReceivedRemoteLinkPlayers)
        {
            task->tState++;
        }
        // The close we asked for was thrown away.
        //
        // LinkCB_WaitCloseLink holds until every player has said they are
        // ready, and a player who closed a frame before us stops saying
        // anything -- so the message can be missed and the wait never ends.
        // Then the partner, already through, opens the battle's link and its
        // player exchange answers with a block send; InitBlockSend overwrites
        // the link callback outright, and our close is simply gone.
        //
        // Asking again would lose the same race again. There is nothing left
        // to close for: the link the partner has just rebuilt is the one the
        // battle is about to use, and they are on the other end of it waiting
        // for us. So take it and go.
        else if (Coop_LinkCloseStage() == 0 && ++task->tEvicted > 30)
        {
            task->tState++;
        }
        else if (++task->tTimer > CLOSE_TIMEOUT_FRAMES)
        {
            // The link would not close. Put the player back rather than leaving
            // them staring at black: the battle cannot start without both
            // consoles, and the session layer can rebuild from here.
            //
            // The party first. It was cut to three before this task ever ran,
            // and giving up here is the one path out that never reaches the
            // battle -- so without this the player walks away permanently
            // three Pokemon lighter.
            if (sReducedParty || sPvpActive)
                LoadPlayerParty();
            sReducedParty = FALSE;
            sSplitTeam = FALSE;

            gCoopDbgBattleBail++;

            Coop_ResumeAfterBattle();
            sCoopBattleActive = FALSE;
            sPvpActive = FALSE;
            SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
            DestroyTask(taskId);
        }
        break;

    case 5:
        // Let the partner finish their own close before opening a new link.
        //
        // The two consoles close within a frame or two of each other, and the
        // first one through goes straight on to open a fresh link for the
        // battle. That new link's handshake lands on a partner who is one
        // frame away from finishing their own close -- and handling it
        // installs a link callback, evicting the close they were in the
        // middle of. They are left holding a link they did not ask for,
        // waiting for a close that can no longer happen, until the timeout
        // sends them back to the overworld. Meanwhile their partner is sat in
        // a battle waiting for a player who has gone home: one black screen,
        // one field, which is exactly what the first attempt at this produced.
        if (++task->tTimer > CLOSE_SETTLE_FRAMES)
        {
            task->tTimer = 0;
            task->tState++;
        }
        break;

    case 6:
        if (sPvpActive)
        {
            // The two players against each other: a plain two-player link
            // battle, which is the one configuration in the game that already
            // means exactly this. No MULTI, no BATTLE_TOWER -- both of those
            // are there to make four players' worth of machinery behave for
            // two, and with two real humans on opposite sides there is nothing
            // to correct.
            PlayMapChosenOrBattleBGM(MUS_VS_TRAINER);

            gBattleTypeFlags = BATTLE_TYPE_LINK | BATTLE_TYPE_TRAINER;

            // The battle needs somebody in the opponent slot even though the
            // opponent's Pokemon arrive over the wire.
            TRAINER_BATTLE_PARAM.opponentA = TRAINER_LINK_OPPONENT;

            CleanupOverworldWindowsAndTilemaps();
            gMain.savedCallback = CB2_ReturnFromPvpBattle;
            SetMainCallback2(CB2_InitBattle);
            DestroyTask(taskId);
            break;
        }

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
#undef tClosedAt
#undef tEvicted

void Coop_StartBattle(u16 opponentA, u16 opponentB, bool8 splitTeam)
{
    TRAINER_BATTLE_PARAM.opponentA = opponentA;
    TRAINER_BATTLE_PARAM.opponentB = opponentB;

    sCoopBattleActive = TRUE;
    sSplitTeam = splitTeam;
    sReducedParty = FALSE;
    sChainedCallback = gMain.savedCallback;

    // Down to the three each player chose.
    //
    // Without a cap the sides are badly lopsided: a leader's six dealt across
    // two slots is still six, while two players turning up with full parties
    // field twelve. Three each puts the same number against that leader as
    // facing them alone would.
    //
    // The choice itself was made before this, by the picker the script command
    // opens; this only applies it. SavePlayerParty was called there too, so the
    // other three are waiting to come back.
    // Only when there is a choice to apply.
    //
    // ReducePlayerPartyToSelectedMons copies the party down to whatever
    // gSelectedOrderFromParty names -- and an empty selection names nothing, so
    // calling it without a picker does not leave the party alone, it empties
    // it. The direct coopbattle_split command opens no picker, and a battle
    // started that way arrived with zero Pokemon on each side.
    if (splitTeam && gSelectedOrderFromParty[0] != 0)
    {
        ReducePlayerPartyToSelectedMons();
        sReducedParty = TRUE;
    }
    CreateTask(Task_CoopBattleStart, 0);
}

void Coop_StartPvpBattle(void)
{
    sPvpActive = TRUE;
    sCoopBattleActive = FALSE;
    sSplitTeam = FALSE;
    sReducedParty = FALSE;

    // Nothing to chain: this battle is not part of a story beat with a script
    // waiting on the other side of it, and the saved callback at this point
    // belongs to whatever last used it.
    sChainedCallback = NULL;

    // Put the party somewhere safe before the battle touches it. The battle
    // itself does the damage; CB2_ReturnFromPvpBattle hands it all back.
    SavePlayerParty();

    // Whole party, whatever each player happens to be carrying. The picker is
    // for the split battles, where a cap is what keeps the fight honest against
    // a gym leader's six -- here both sides are a player's own team and
    // whatever they have is, by definition, a fair fight between them.
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

// Whether the party picker was opened for the battle being set up, and so
// whether there is a choice to check and a partner to wait for.
static EWRAM_DATA bool8 sPickerOpened = FALSE;

void Coop_SetPickerOpened(bool8 opened)
{
    sPickerOpened = opened;
}

bool8 Coop_PickerWasOpened(void)
{
    return sPickerOpened;
}

void Coop_CancelNextBattle(void)
{
    sNextMarked = FALSE;
    sPickerOpened = FALSE;
}
