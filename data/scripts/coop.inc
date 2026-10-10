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
CoopEventScript_ChooseStarter::
	dynmultipush CoopText_Kanto,  0
	dynmultipush CoopText_Johto,  1
	dynmultipush CoopText_Hoenn,  2
	dynmultipush CoopText_Sinnoh, 3
	dynmultipush CoopText_Unova,  4
	dynmultipush CoopText_Kalos,  5
	dynmultipush CoopText_Alola,  6
	dynmultipush CoopText_Galar,  7
	dynmultipush CoopText_Paldea, 8
	dynmultistack 0, 0, TRUE, 6, FALSE, 0, NULL
	switch VAR_RESULT
	case 0, CoopEventScript_StartersKanto
	case 1, CoopEventScript_StartersJohto
	case 2, CoopEventScript_StartersHoenn
	case 3, CoopEventScript_StartersSinnoh
	case 4, CoopEventScript_StartersUnova
	case 5, CoopEventScript_StartersKalos
	case 6, CoopEventScript_StartersAlola
	case 7, CoopEventScript_StartersGalar
	case 8, CoopEventScript_StartersPaldea
	@ B out of the region menu and you are asked again. There is no way on
	@ from here without a Pokemon.
	goto CoopEventScript_ChooseStarter

CoopEventScript_StartersKanto:
	setvar VAR_TEMP_B, SPECIES_BULBASAUR
	setvar VAR_TEMP_C, SPECIES_CHARMANDER
	setvar VAR_TEMP_D, SPECIES_SQUIRTLE
	dynmultipush CoopText_Bulbasaur, 0
	dynmultipush CoopText_Charmander, 1
	dynmultipush CoopText_Squirtle, 2
	goto CoopEventScript_PickStarter

CoopEventScript_StartersJohto:
	setvar VAR_TEMP_B, SPECIES_CHIKORITA
	setvar VAR_TEMP_C, SPECIES_CYNDAQUIL
	setvar VAR_TEMP_D, SPECIES_TOTODILE
	dynmultipush CoopText_Chikorita, 0
	dynmultipush CoopText_Cyndaquil, 1
	dynmultipush CoopText_Totodile, 2
	goto CoopEventScript_PickStarter

CoopEventScript_StartersHoenn:
	setvar VAR_TEMP_B, SPECIES_TREECKO
	setvar VAR_TEMP_C, SPECIES_TORCHIC
	setvar VAR_TEMP_D, SPECIES_MUDKIP
	dynmultipush CoopText_Treecko, 0
	dynmultipush CoopText_Torchic, 1
	dynmultipush CoopText_Mudkip, 2
	goto CoopEventScript_PickStarter

CoopEventScript_StartersSinnoh:
	setvar VAR_TEMP_B, SPECIES_TURTWIG
	setvar VAR_TEMP_C, SPECIES_CHIMCHAR
	setvar VAR_TEMP_D, SPECIES_PIPLUP
	dynmultipush CoopText_Turtwig, 0
	dynmultipush CoopText_Chimchar, 1
	dynmultipush CoopText_Piplup, 2
	goto CoopEventScript_PickStarter

CoopEventScript_StartersUnova:
	setvar VAR_TEMP_B, SPECIES_SNIVY
	setvar VAR_TEMP_C, SPECIES_TEPIG
	setvar VAR_TEMP_D, SPECIES_OSHAWOTT
	dynmultipush CoopText_Snivy, 0
	dynmultipush CoopText_Tepig, 1
	dynmultipush CoopText_Oshawott, 2
	goto CoopEventScript_PickStarter

CoopEventScript_StartersKalos:
	setvar VAR_TEMP_B, SPECIES_CHESPIN
	setvar VAR_TEMP_C, SPECIES_FENNEKIN
	setvar VAR_TEMP_D, SPECIES_FROAKIE
	dynmultipush CoopText_Chespin, 0
	dynmultipush CoopText_Fennekin, 1
	dynmultipush CoopText_Froakie, 2
	goto CoopEventScript_PickStarter

CoopEventScript_StartersAlola:
	setvar VAR_TEMP_B, SPECIES_ROWLET
	setvar VAR_TEMP_C, SPECIES_LITTEN
	setvar VAR_TEMP_D, SPECIES_POPPLIO
	dynmultipush CoopText_Rowlet, 0
	dynmultipush CoopText_Litten, 1
	dynmultipush CoopText_Popplio, 2
	goto CoopEventScript_PickStarter

CoopEventScript_StartersGalar:
	setvar VAR_TEMP_B, SPECIES_GROOKEY
	setvar VAR_TEMP_C, SPECIES_SCORBUNNY
	setvar VAR_TEMP_D, SPECIES_SOBBLE
	dynmultipush CoopText_Grookey, 0
	dynmultipush CoopText_Scorbunny, 1
	dynmultipush CoopText_Sobble, 2
	goto CoopEventScript_PickStarter

CoopEventScript_StartersPaldea:
	setvar VAR_TEMP_B, SPECIES_SPRIGATITO
	setvar VAR_TEMP_C, SPECIES_FUECOCO
	setvar VAR_TEMP_D, SPECIES_QUAXLY
	dynmultipush CoopText_Sprigatito, 0
	dynmultipush CoopText_Fuecoco, 1
	dynmultipush CoopText_Quaxly, 2
	goto CoopEventScript_PickStarter

@ The three the region above pushed, with the species themselves in
@ VAR_TEMP_B/C/D so one copy of this serves all nine.
@
@ B goes back to the region list rather than out: there is nothing on the
@ other side of this script but a player with no Pokemon.
CoopEventScript_PickStarter:
	dynmultistack 0, 0, FALSE, 3, FALSE, 0, NULL
	switch VAR_RESULT
	case 0, CoopEventScript_TakeFirst
	case 1, CoopEventScript_TakeSecond
	case 2, CoopEventScript_TakeThird
	goto CoopEventScript_ChooseStarter

CoopEventScript_TakeFirst:
	copyvar VAR_TEMP_E, VAR_TEMP_B
	setvar VAR_TEMP_F, 0
	goto CoopEventScript_GiveStarter

CoopEventScript_TakeSecond:
	copyvar VAR_TEMP_E, VAR_TEMP_C
	setvar VAR_TEMP_F, 1
	goto CoopEventScript_GiveStarter

CoopEventScript_TakeThird:
	copyvar VAR_TEMP_E, VAR_TEMP_D
	setvar VAR_TEMP_F, 2

@ No waitstate after the callnative. ScriptGiveMon returns having done the
@ work; waitstate blocks until something calls ScriptContext_Enable, and a
@ plain callnative sets up no such wait -- so the script sat there for ever
@ with the player locked, one line short of finishing.
CoopEventScript_GiveStarter:
	callnative Coop_GiveChosenStarter
	msgbox CoopText_StarterChosen, MSGBOX_DEFAULT
	@ Whoever chose first waits here. Without it the faster player walks on
	@ into the rest of Birch's scene while their partner is still reading a
	@ list of regions, and the two spend the next conversation a beat apart.
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
	.string "go and find out what's going on.$"

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
