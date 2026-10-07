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

@ For the scene test in the debug menu's Scripts list.
CoopText_TestBothHere:
	.string "You're both here.\p"
	.string "Neither of you got this far\n"
	.string "alone.$"

CoopText_TestStillTogether:
	.string "And still together after a\n"
	.string "second gate.$"
