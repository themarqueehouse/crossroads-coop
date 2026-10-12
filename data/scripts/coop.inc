@ Co-op scripts.
@
@ Reached when a scene that needs both players is triggered while the other
@ player is somewhere else. Nothing in the scene has run at this point, so
@ saying so and ending is all there is to do -- the trigger is still unfired and
@ will run properly once they are both here.

CoopEventScript_PartnerNotHere::
	lockall
	msgbox CoopText_PartnerNotHere, MSGBOX_DEFAULT
	releaseall
	end

CoopText_PartnerNotHere:
	.string "This won't happen without your\n"
	.string "partner here.$"

@ Shown while sat at a sync gate. Not through a script: the gate is a native
@ wait, so src/coop.c puts this up itself and takes it down when the gate opens.
CoopText_WaitingForPartner::
	.string "Waiting for your partner…$"

@ For the scene test in the debug menu's Scripts list.
CoopText_TestBothHere:
	.string "You're both here.\p"
	.string "Neither of you got this far\n"
	.string "alone.$"

CoopText_TestStillTogether:
	.string "And still together after a\n"
	.string "second gate.$"

@ ---------------------------------------------------------------------------
@ Battling each other.
@
@ "How do you battle each other?" -- you walk up to them and press A. That is
@ the first place anyone would try, and the other player is the one thing in
@ the world that is actually standing there.
@
@ It also stops a crash. The partner is a spawned object with no map template
@ behind it, and the ordinary interaction path looks a script up through that
@ template -- so before this, pressing A at your partner read a script pointer
@ out of nothing and ran whatever it found.
@
@ The offer is mirrored, so both consoles ask. Each player then answers for
@ themselves, because a battle that began when your friend pressed A at your
@ back is not something you agreed to. Whoever says yes waits at the gate; a
@ no never reports it, and the keen one is let go when it times out.
@ ---------------------------------------------------------------------------
CoopEventScript_BattlePartner::
	coopscene GATE_COOP_PVP
	lockall
	msgbox CoopText_PvpOffer, MSGBOX_YESNO
	goto_if_eq VAR_RESULT, NO, CoopEventScript_PvpDeclined
	coopgate_or_give_up GATE_COOP_PVP_READY
	cooppvp
	releaseall
	end

CoopEventScript_PvpDeclined:
	msgbox CoopText_PvpDeclined, MSGBOX_DEFAULT
	releaseall
	end

@ Their follower Pokemon, which is also a spawned object with no template and
@ so also had nothing to look a script up in. Nothing to do here but say
@ something -- it is not yours, and it is not going to battle you.
CoopEventScript_PartnersPokemon::
	lockall
	msgbox CoopText_PartnersPokemon, MSGBOX_DEFAULT
	releaseall
	end

@ One page, deliberately. A \p needs a button press to turn it, and the press
@ comes before the Yes/No rather than after -- so the box sat there looking
@ answered while the answer went into the paragraph break.
CoopText_PvpOffer:
	.string "Battle each other? You both have\n"
	.string "to say yes.$"

CoopText_PvpDeclined:
	.string "Not right now.$"

CoopText_PartnersPokemon:
	.string "This one is your partner's.\p"
	.string "It doesn't look like it wants\n"
	.string "anything to do with you.$"

@ ---------------------------------------------------------------------------
@ First run: who you are, and which Pokemon you start with.
@
@ What the opening would have asked, asked here instead. Two players skipping
@ the van and Birch's speech still have to be somebody and still need
@ something to battle with, and the opening is the only place the vanilla game
@ ever asks -- so a co-op game that skips it has to ask on its own.
@
@ Each console runs this for itself. It is deliberately NOT a mirrored scene:
@ one each, chosen separately, and two players picking the same starter is
@ allowed. Mirroring it would mean watching your partner scroll a menu.
@
@ Starters from every region, which is what was asked for. Nine regions is too
@ many to put on one screen, so it is region first and then its three.
@ ---------------------------------------------------------------------------

@ Who you are. Asked once, on each console, the first time you play.
@
@ The starter is NOT asked here any more. It used to be, and that put the
@ choice in a strange place -- standing in your home town before anything has
@ happened -- and worse, it meant Birch still had a starter to give you when
@ you reached his bag on Route 101 and asked you to choose a second time. The
@ story has its own moment for this and it is already a shared scene, so the
@ choice lives there now. See CoopEventScript_ChooseStarter.
CoopEventScript_FirstRun::
	lockall
	msgbox CoopText_FirstRunWelcome, MSGBOX_DEFAULT

CoopEventScript_FirstRunName:
	special CoopDoPlayerNamingScreen
	waitstate

	@ No message box in front of the menu. "Boy / Girl" says what it is, and
	@ a box before it is a press that does nothing but give the player
	@ somewhere else to be out of step.
	dynmultipush CoopText_Boy, 0
	dynmultipush CoopText_Girl, 1
	dynmultistack 0, 0, TRUE, 2, FALSE, 0, NULL
	switch VAR_RESULT
	case 1, CoopEventScript_FirstRunGirl
	callnative Coop_SetGenderMale
	goto CoopEventScript_FirstRunDone

CoopEventScript_FirstRunGirl:
	callnative Coop_SetGenderFemale

CoopEventScript_FirstRunDone:
	msgbox CoopText_FirstRunReady, MSGBOX_DEFAULT
	callnative Coop_FirstRunDone
	releaseall
	end

@ The starter, from every region, at Birch's bag on Route 101.
@
@ Called from the middle of that scene in place of the game's own
@ ChooseStarter, which offers the three Hoenn ones. The scene is mirrored, so
@ both players are standing here and both choose; a gate afterwards holds
@ whoever picks first until the other has, so the rest of the scene runs for
@ the two of them together.
@ All nine regions' starters, on the game's own starter screen.
@
@ It used to be two text menus -- pick a region, then pick one of its three --
@ which named the Pokemon and showed nothing. Asked for directly: a picture of
@ the one being chosen, and not being made to pick a region first. So this uses
@ the screen Birch's bag uses, which already draws the three Pokeballs, the
@ front sprite and the name, with up and down turning between regions. The
@ species comes back in VAR_TEMP_E and which of the three it was in VAR_TEMP_F.
CoopEventScript_ChooseStarter::
	special CoopChooseStarterAllRegions
	waitstate
CoopEventScript_GiveStarter:
	callnative Coop_GiveChosenStarter
	msgbox CoopText_StarterChosen, MSGBOX_DEFAULT
	@ Whoever chose first waits here. Without it the faster player walks on
	@ into the rest of Birch's scene while their partner is still looking
	@ through the regions, and the two spend the next conversation a beat
	@ apart.
	coopgate GATE_STORY_STARTERS_PICKED
	return

CoopText_FirstRunWelcome:
	.string "Before you set off - who are you,\n"
	.string "and who is coming with you?$"

CoopText_FirstRunGender:
	.string "Are you a boy or a girl?$"

CoopText_FirstRunPickRegion:
	.string "Every region sent a starter.\n"
	.string "Which one's?$"

CoopText_FirstRunReady:
	.string "Then you're ready.\p"
	.string "Your partner is out here too -\n"
	.string "go and find out what's going on.\p"
	.string "Walk up to them and press A if you\n"
	.string "ever want to battle each other.$"

CoopText_StarterChosen:
	.string "Your partner is picking theirs -\n"
	.string "you might end up with the same one.$"

CoopText_Boy:
	.string "Boy$"
CoopText_Girl:
	.string "Girl$"

CoopText_Kanto:
	.string "Kanto$"
CoopText_Johto:
	.string "Johto$"
CoopText_Hoenn:
	.string "Hoenn$"
CoopText_Sinnoh:
	.string "Sinnoh$"
CoopText_Unova:
	.string "Unova$"
CoopText_Kalos:
	.string "Kalos$"
CoopText_Alola:
	.string "Alola$"
CoopText_Galar:
	.string "Galar$"
CoopText_Paldea:
	.string "Paldea$"

CoopText_Bulbasaur:
	.string "BULBASAUR$"
CoopText_Charmander:
	.string "CHARMANDER$"
CoopText_Squirtle:
	.string "SQUIRTLE$"
CoopText_Chikorita:
	.string "CHIKORITA$"
CoopText_Cyndaquil:
	.string "CYNDAQUIL$"
CoopText_Totodile:
	.string "TOTODILE$"
CoopText_Treecko:
	.string "TREECKO$"
CoopText_Torchic:
	.string "TORCHIC$"
CoopText_Mudkip:
	.string "MUDKIP$"
CoopText_Turtwig:
	.string "TURTWIG$"
CoopText_Chimchar:
	.string "CHIMCHAR$"
CoopText_Piplup:
	.string "PIPLUP$"
CoopText_Snivy:
	.string "SNIVY$"
CoopText_Tepig:
	.string "TEPIG$"
CoopText_Oshawott:
	.string "OSHAWOTT$"
CoopText_Chespin:
	.string "CHESPIN$"
CoopText_Fennekin:
	.string "FENNEKIN$"
CoopText_Froakie:
	.string "FROAKIE$"
CoopText_Rowlet:
	.string "ROWLET$"
CoopText_Litten:
	.string "LITTEN$"
CoopText_Popplio:
	.string "POPPLIO$"
CoopText_Grookey:
	.string "GROOKEY$"
CoopText_Scorbunny:
	.string "SCORBUNNY$"
CoopText_Sobble:
	.string "SOBBLE$"
CoopText_Sprigatito:
	.string "SPRIGATITO$"
CoopText_Fuecoco:
	.string "FUECOCO$"
CoopText_Quaxly:
	.string "QUAXLY$"
