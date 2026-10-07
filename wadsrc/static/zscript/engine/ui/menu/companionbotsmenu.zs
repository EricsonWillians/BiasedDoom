//=============================================================================
//
// Companion squad menus.
//
// The engine owns the actual profile and network state. Keep this UI as a
// small transactional surface over that state: a player creates one draft,
// customizes it, then explicitly confirms it. That makes the zero-to-one path
// obvious and avoids an unconfigured random bot appearing before the player
// has had a chance to choose its appearance.
//=============================================================================

// OptionMenuItemStaticText deliberately draws a single line. Keep companion
// guidance short and explicitly split it where needed, rather than letting a
// long help sentence clip at classic 320-wide menu scales.
class CompanionOptionMenu : OptionMenu
{
	void AddHeading(String text)
	{
		mDesc.mItems.Push(new("OptionMenuItemStaticText").Init(text, 1));
	}

	void AddInfo(String first, String second = "")
	{
		mDesc.mItems.Push(new("OptionMenuItemStaticText").Init(first, 0));
		if (!second.IsEmpty())
			mDesc.mItems.Push(new("OptionMenuItemStaticText").Init(second, 0));
	}

	void AddSpacer()
	{
		mDesc.mItems.Push(new("OptionMenuItemStaticText").Init("", 0));
	}
}

class CompanionProfileCycleItem : OptionMenuItem
{
	const KIND_IDENTITY = 0;
	const KIND_SKIN = 1;
	const KIND_STYLE = 2;
	const KIND_SKILL = 3;

	int mProfile;
	int mKind;

	CompanionProfileCycleItem Init(String label, int profile, int kind)
	{
		Super.Init(label, 'None', false);
		mProfile = profile;
		mKind = kind;
		return self;
	}

	override int Draw(OptionMenuDescriptor desc, int y, int indent, bool selected)
	{
		bool editable = CanEditProfile();
		drawLabel(indent, y, selected && editable ? OptionMenuSettings.mFontColorSelection : OptionMenuSettings.mFontColor, !editable);
		drawValue(indent, y, OptionMenuSettings.mFontColorValue, ValueText(), !editable, false);
		return indent;
	}

	override bool Selectable()
	{
		return CanEditProfile();
	}

	override bool MenuEvent(int mkey, bool fromcontroller)
	{
		if (!CanEditProfile())
			return true;

		int count = ChoiceCount();
		if (count <= 0)
			return Super.MenuEvent(mkey, fromcontroller);

		int selection = Selection();
		int direction;
		if (mkey == Menu.MKEY_Left)
		{
			direction = -1;
		}
		else if (mkey == Menu.MKEY_Right || mkey == Menu.MKEY_Enter)
		{
			direction = 1;
		}
		else
		{
			return Super.MenuEvent(mkey, fromcontroller);
		}

		// A roster can change while a network host is looking at the menu. Keep
		// cycling until native validation accepts a real available choice.
		for (int attempts = 0; attempts < count; ++attempts)
		{
			selection = (selection + direction + count) % count;
			if (SetSelection(selection))
			{
				Menu.MenuSound("menu/change");
				return true;
			}
		}

		if (mKind == KIND_IDENTITY)
			Menu.StartMessage("No identity could be selected. It may be in use, or the network may be busy; try again.", 1);
		else
			Menu.StartMessage("Could not save that setting. The network may be busy; try again.", 1);
		return true;
	}

	private int ChoiceCount()
	{
		switch (mKind)
		{
		case KIND_IDENTITY: return CompanionBots.GetRosterCount() + 1; // 0 = choose available identity on deploy
		case KIND_SKIN: return CompanionBots.GetSkinCount() + 1; // 0 = class default
		case KIND_STYLE: return 12; // roster default plus the eleven stable colour styles
		default: return 4; // roster default plus three sensible combat presets
		}
	}

	private bool CanEditProfile()
	{
		// Static profile menus can still be reached through a saved menu state.
		// Do not let that accidentally edit an inactive profile; the only valid
		// editing contexts are a deployed squad member and the one open draft.
		return CompanionBots.CanManage() &&
			(CompanionBots.IsProfileEnabled(mProfile) || CompanionBots.GetDraftProfile() == mProfile);
	}

	private int Selection()
	{
		String current;
		if (mKind == KIND_IDENTITY)
		{
			current = CompanionBots.GetProfileName(mProfile);
			for (int i = 0; i < CompanionBots.GetRosterCount(); ++i)
			{
				if (current ~== CompanionBots.GetRosterName(i))
					return i + 1;
			}
			return 0;
		}
		if (mKind == KIND_SKIN)
		{
			current = CompanionBots.GetProfileSkin(mProfile);
			for (int i = 0; i < CompanionBots.GetSkinCount(); ++i)
			{
				if (current ~== CompanionBots.GetSkinName(i))
					return i + 1;
			}
			return 0;
		}
		if (mKind == KIND_STYLE)
			return clamp(CompanionBots.GetProfileStyle(mProfile), 0, 11);
		return clamp(CompanionBots.GetProfileSkill(mProfile), 0, 3);
	}

	private bool SetSelection(int selection)
	{
		if (mKind == KIND_IDENTITY)
		{
			return CompanionBots.SetProfileName(mProfile,
				selection == 0 ? "" : CompanionBots.GetRosterName(selection - 1));
		}
		if (mKind == KIND_SKIN)
		{
			return CompanionBots.SetProfileSkin(mProfile,
				selection == 0 ? "" : CompanionBots.GetSkinName(selection - 1));
		}
		if (mKind == KIND_STYLE)
			return CompanionBots.SetProfileStyle(mProfile, selection);
		return CompanionBots.SetProfileSkill(mProfile, selection);
	}

	private String ValueText()
	{
		if (mKind == KIND_IDENTITY)
		{
			String profileName = CompanionBots.GetProfileName(mProfile);
			return profileName.Length() == 0 ? "Choose available on deploy" : profileName;
		}
		if (mKind == KIND_SKIN)
		{
			String skin = CompanionBots.GetProfileSkin(mProfile);
			return skin.Length() == 0 ? "Class default" : skin;
		}
		if (mKind == KIND_STYLE)
			return CompanionBots.GetStyleName(CompanionBots.GetProfileStyle(mProfile));
		return CompanionBots.GetSkillName(CompanionBots.GetProfileSkill(mProfile));
	}
}

class CompanionActionItem : OptionMenuItem
{
	override int Draw(OptionMenuDescriptor desc, int y, int indent, bool selected)
	{
		bool enabled = Selectable();
		drawLabel(indent, y, selected && enabled ? OptionMenuSettings.mFontColorSelection : OptionMenuSettings.mFontColor, !enabled);
		return indent;
	}
}

class CompanionProfileRemoveItem : CompanionActionItem
{
	int mProfile;

	CompanionProfileRemoveItem Init(int profile)
	{
		Super.Init("Remove this companion", 'None', false);
		mProfile = profile;
		return self;
	}

	override bool Selectable()
	{
		return CompanionBots.CanManage() && CompanionBots.IsProfileEnabled(mProfile);
	}

	override bool MenuEvent(int mkey, bool fromcontroller)
	{
		if (mkey == Menu.MKEY_MBYes)
			return CommitRemoval();
		return Super.MenuEvent(mkey, fromcontroller);
	}

	override bool Activate()
	{
		String displayName = CompanionBots.GetProfileLiveName(mProfile);
		if (displayName.IsEmpty())
			displayName = CompanionBots.GetProfileName(mProfile);
		if (displayName.IsEmpty())
			displayName = String.Format("Companion %d", mProfile + 1);
		String prompt = String.Format("%s%s%s will be removed from this squad. Continue?",
			TEXTCOLOR_WHITE, displayName, TEXTCOLOR_NORMAL);
		Menu.StartMessage(prompt, 0);
		return true;
	}

	private bool CommitRemoval()
	{
		if (CompanionBots.RemoveProfile(mProfile))
		{
			Menu.MenuSound("menu/choose");
		}
		else
		{
			// This callback is invoked while the confirmation message box remains
			// current. Do not open or close another menu here: the box will safely
			// return to this page, where normal state refresh can keep the player
			// in control and let them retry.
			Menu.MenuSound("menu/backup");
		}
		return true;
	}
}

class CompanionProfileMenu : CompanionOptionMenu
{
	int mProfile;
	int mState;
	bool mEnabled;
	bool mCanManage;
	bool mRemovalFailurePending;

	virtual int GetProfileSlot()
	{
		return 0;
	}

	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		Super.Init(parent, desc);
		mProfile = GetProfileSlot();
		Build();
	}

	void Build()
	{
		mDesc.mItems.Clear();

		mState = CompanionBots.GetProfileState(mProfile);
		mEnabled = CompanionBots.IsProfileEnabled(mProfile);
		mCanManage = CompanionBots.CanManage();
		mRemovalFailurePending = false;
		String state = CompanionBots.GetProfileStateText(mProfile);
		AddHeading(String.Format("Companion %d — %s", mProfile + 1, state));
		if (!mEnabled)
		{
			AddInfo("This slot is not in the squad.");
		}
		else if (mState == 2)
		{
			AddInfo("Changes apply next time", "this companion joins.");
		}
		else
		{
			AddInfo("These settings apply when", "this companion joins.");
		}
		AddSpacer();

		let identity = new("CompanionProfileCycleItem").Init("Identity", mProfile, 0);
		let skin = new("CompanionProfileCycleItem").Init("Skin override", mProfile, 1);
		let style = new("CompanionProfileCycleItem").Init("Appearance style", mProfile, 2);
		let skill = new("CompanionProfileCycleItem").Init("Combat skill", mProfile, 3);
		mDesc.mItems.Push(identity);
		mDesc.mItems.Push(skin);
		mDesc.mItems.Push(style);
		mDesc.mItems.Push(skill);
		AddSpacer();
		mDesc.mItems.Push(new("CompanionProfileRemoveItem").Init(mProfile));
		AddInfo("Removes this profile only.", "The other companions stay.");

		if (!mCanManage)
			AddInfo("Only the host or local controller", "can edit this squad.");

		mDesc.mSelectedItem = FirstSelectable();
		mDesc.CalcIndent();
	}

	override void Ticker()
	{
		Super.Ticker();
		if (mRemovalFailurePending)
		{
			mRemovalFailurePending = false;
			Menu.StartMessage("The removal could not be queued. Try again.", 1);
			return;
		}
		// The confirmation result is delivered before its message box closes.
		// Once this page becomes current again, return to the squad list rather
		// than leaving stale controls for a profile that no longer exists.
		if (mEnabled && !CompanionBots.IsProfileEnabled(mProfile))
		{
			Close();
			return;
		}
		if (mState != CompanionBots.GetProfileState(mProfile) ||
			mEnabled != CompanionBots.IsProfileEnabled(mProfile) ||
			mCanManage != CompanionBots.CanManage())
		{
			Build();
		}
	}

	override bool MenuEvent(int mkey, bool fromcontroller)
	{
		bool confirmingRemoval = mkey == Menu.MKEY_MBYes &&
			mDesc.mSelectedItem >= 0 &&
			mDesc.mItems[mDesc.mSelectedItem] is "CompanionProfileRemoveItem";
		bool handled = Super.MenuEvent(mkey, fromcontroller);
		if (confirmingRemoval && CompanionBots.IsProfileEnabled(mProfile))
		{
			// A Yes/No message box dispatches this event before closing itself.
			// Defer user-facing failure feedback to Ticker(), when this profile page
			// has safely regained menu ownership.
			mRemovalFailurePending = true;
		}
		return handled;
	}
}

class CompanionProfile1Menu : CompanionProfileMenu { override int GetProfileSlot() { return 0; } }
class CompanionProfile2Menu : CompanionProfileMenu { override int GetProfileSlot() { return 1; } }
class CompanionProfile3Menu : CompanionProfileMenu { override int GetProfileSlot() { return 2; } }
class CompanionProfile4Menu : CompanionProfileMenu { override int GetProfileSlot() { return 3; } }
class CompanionProfile5Menu : CompanionProfileMenu { override int GetProfileSlot() { return 4; } }
class CompanionProfile6Menu : CompanionProfileMenu { override int GetProfileSlot() { return 5; } }
class CompanionProfile7Menu : CompanionProfileMenu { override int GetProfileSlot() { return 6; } }

class CompanionDraftDeployItem : CompanionActionItem
{
	CompanionDraftDeployItem Init()
	{
		Super.Init(CompanionBots.CanDeploy() ? "Add companion to squad" : "Save companion to squad", 'None', false);
		return self;
	}

	override bool Selectable()
	{
		return CompanionBots.CanManage() && CompanionBots.GetDraftProfile() >= 0;
	}

	override bool Activate()
	{
		int profile = CompanionBots.DeployDraft();
		if (profile >= 0)
		{
			Menu.MenuSound("menu/choose");
			let current = Menu.GetCurrentMenu();
			if (current != null)
				current.Close();
		}
		else
		{
			Menu.StartMessage(CompanionBots.CanDeploy() ?
				"Could not add this companion. Check the roster or try again if the network is busy." :
				"Could not save this companion. Check the roster or try again if the network is busy.", 1);
		}
		return true;
	}
}

class CompanionDraftCancelItem : CompanionActionItem
{
	CompanionDraftCancelItem Init()
	{
		Super.Init("Discard this draft", 'None', false);
		return self;
	}

	override bool Selectable()
	{
		return CompanionBots.CanManage() && CompanionBots.GetDraftProfile() >= 0;
	}

	override bool Activate()
	{
		CompanionBots.CancelDraft();
		Menu.MenuSound("menu/back");
		let current = Menu.GetCurrentMenu();
		if (current != null)
			current.Close();
		return true;
	}
}

class CompanionDraftRandomizeItem : CompanionActionItem
{
	CompanionDraftRandomizeItem Init()
	{
		Super.Init("Randomize this configuration", 'None', false);
		return self;
	}

	override bool Selectable()
	{
		return CompanionBots.CanManage() && CompanionBots.GetDraftProfile() >= 0;
	}

	override bool Activate()
	{
		if (CompanionBots.RandomizeDraft())
		{
			Menu.MenuSound("menu/change");
		}
		else
		{
			Menu.StartMessage("Could not randomize this companion. Its identity may no longer be available, or the network may be busy; try again.", 1);
		}
		return true;
	}
}

class CompanionDraftMenu : CompanionOptionMenu
{
	int mProfile;

	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		Super.Init(parent, desc);
		mDesc.mItems.Clear();
		mProfile = CompanionBots.GetDraftProfile();
		if (mProfile < 0)
		{
			AddHeading("No companion draft is active.");
			AddInfo("Return to the squad roster, then", "choose Add Companion.");
			mDesc.mSelectedItem = -1;
			mDesc.CalcIndent();
			return;
		}

		AddHeading(String.Format("Configure Companion %d", mProfile + 1));
		// The Identity row below is the live source of truth. Keep this guidance
		// true both before and after cycling it, rather than rebuilding the menu
		// (and moving focus) on every left/right selection.
		AddInfo("Choose a library identity, or let", "the game choose an available one.");
		if (!CompanionBots.CanDeploy())
			AddInfo("It will join your next", "cooperative map.");
		AddSpacer();
		mDesc.mItems.Push(new("CompanionProfileCycleItem").Init("Identity", mProfile, 0));
		mDesc.mItems.Push(new("CompanionProfileCycleItem").Init("Skin override", mProfile, 1));
		mDesc.mItems.Push(new("CompanionProfileCycleItem").Init("Appearance style", mProfile, 2));
		mDesc.mItems.Push(new("CompanionProfileCycleItem").Init("Combat skill", mProfile, 3));
		mDesc.mItems.Push(new("CompanionDraftRandomizeItem").Init());
		AddInfo("Random picks a usable identity,", "skin/default, style, and skill.");
		AddSpacer();
		mDesc.mItems.Push(new("CompanionDraftDeployItem").Init());
		mDesc.mItems.Push(new("CompanionDraftCancelItem").Init());

		mDesc.mSelectedItem = FirstSelectable();
		mDesc.CalcIndent();
	}

	override bool MenuEvent(int mkey, bool fromcontroller)
	{
		if (mkey == Menu.MKEY_Abort && CompanionBots.GetDraftProfile() == mProfile)
			CompanionBots.CancelDraft();
		return Super.MenuEvent(mkey, fromcontroller);
	}

	override void OnDestroy()
	{
		// A menu can be closed by more than the explicit Back event (for example
		// when a game transition tears down menus). Never leave a half-configured
		// draft behind to surprise the next visit to the roster.
		if (CompanionBots.GetDraftProfile() == mProfile)
			CompanionBots.CancelDraft();
		Super.OnDestroy();
	}
}

class CompanionRosterItem : OptionMenuItem
{
	String mStatus;

	CompanionRosterItem Init(String label, String status)
	{
		Super.Init(label, 'None', false);
		mStatus = status;
		return self;
	}

	override int Draw(OptionMenuDescriptor desc, int y, int indent, bool selected)
	{
		drawLabel(indent, y, selected ? OptionMenuSettings.mFontColorSelection : OptionMenuSettings.mFontColor, false);
		drawValue(indent, y, OptionMenuSettings.mFontColorValue, mStatus, false, false);
		return indent;
	}

	override bool Selectable()
	{
		// These are read-only rows, but they still need focus. It lets a long
		// bots.cfg library scroll through ordinary keyboard/controller movement
		// without pretending that Enter changes a companion.
		return true;
	}

	override bool Activate()
	{
		return true;
	}
}

class CompanionRosterMenu : CompanionOptionMenu
{
	int mStateSignature;

	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		Super.Init(parent, desc);
		Build();
	}

	int StateSignature()
	{
		bool authoritative = CompanionBots.CanManage();
		int count = CompanionBots.GetRosterCount();
		int signature = count * 17 + (authoritative ? 1 : 0);
		if (authoritative)
		{
			for (int i = 0; i < count; ++i)
				signature = signature * 3 + CompanionBots.GetRosterState(i);
		}
		return signature;
	}

	void Build()
	{
		mDesc.mItems.Clear();
		AddHeading("Identity library");
		bool authoritative = CompanionBots.CanManage();
		if (authoritative)
			AddInfo("Choose an identity while", "creating a companion.");
		else
			AddInfo("Host identity availability is", "reference-only for guests.");
		AddSpacer();

		int count = CompanionBots.GetRosterCount();
		if (count == 0)
		{
			AddInfo("No companion identities are available.");
		}
		else
		{
			for (int i = 0; i < count; ++i)
			{
				int state = CompanionBots.GetRosterState(i);
				// A guest may have a different local bots.cfg, so its roster cannot
				// authoritatively identify the host's available names. The main squad
				// roster still shows replicated profile state; avoid claiming that a
				// library entry is available here.
				String status = authoritative ? (state == 2 ? "Active" : state == 1 ? "Joining" : "Available") : "Host-controlled";
				mDesc.mItems.Push(new("CompanionRosterItem").Init(CompanionBots.GetRosterName(i), status));
			}
		}
		mStateSignature = StateSignature();
		mDesc.mSelectedItem = FirstSelectable();
		mDesc.CalcIndent();
	}

	override void Ticker()
	{
		Super.Ticker();
		if (mStateSignature != StateSignature())
			Build();
	}
}

class CompanionProfileLinkItem : OptionMenuItemSubmenu
{
	int mProfile;

	CompanionProfileLinkItem Init(int profile)
	{
		mProfile = profile;
		Super.Init(String.Format("Companion %d", profile + 1), ProfileMenuName());
		return self;
	}

	override int Draw(OptionMenuDescriptor desc, int y, int indent, bool selected)
	{
		drawLabel(indent, y, selected ? OptionMenuSettings.mFontColorSelection : OptionMenuSettings.mFontColorMore);
		String displayName = CompanionBots.GetProfileLiveName(mProfile);
		if (displayName.Length() == 0)
			displayName = CompanionBots.GetProfileName(mProfile);
		if (displayName.Length() == 0)
			displayName = "Ready to choose";
		drawValue(indent, y, OptionMenuSettings.mFontColorValue,
			String.Format("%s — %s", displayName, CompanionBots.GetProfileStateText(mProfile)), false, false);
		return indent;
	}

	private Name ProfileMenuName()
	{
		switch (mProfile)
		{
		case 0: return 'CompanionProfile1Menu';
		case 1: return 'CompanionProfile2Menu';
		case 2: return 'CompanionProfile3Menu';
		case 3: return 'CompanionProfile4Menu';
		case 4: return 'CompanionProfile5Menu';
		case 5: return 'CompanionProfile6Menu';
		default: return 'CompanionProfile7Menu';
		}
	}
}

class CompanionBeginDraftItem : CompanionActionItem
{
	CompanionBeginDraftItem Init()
	{
		Super.Init("Add Companion…", 'None', false);
		return self;
	}

	override bool Selectable()
	{
		// Do not let the player enter a draft that can never resolve. This checks
		// both a free numbered profile and an unused identity, while still
		// allowing a squad to be prepared when this map has no co-op place yet.
		return CompanionBots.CanBeginDraft();
	}

	override bool Activate()
	{
		if (CompanionBots.BeginDraft() >= 0)
		{
			Menu.MenuSound("menu/advance");
			Menu.SetMenu('CompanionDraftMenu');
		}
		else
		{
			Menu.StartMessage("Could not start a draft. The roster may be full, or the network may be busy; try again.", 1);
		}
		return true;
	}
}

class CompanionBeginRandomDraftItem : CompanionActionItem
{
	CompanionBeginRandomDraftItem Init()
	{
		Super.Init("Add Random Companion…", 'None', false);
		return self;
	}

	override bool Selectable()
	{
		return CompanionBots.CanBeginDraft();
	}

	override bool Activate()
	{
		if (CompanionBots.BeginRandomDraft() >= 0)
		{
			Menu.MenuSound("menu/advance");
			Menu.SetMenu('CompanionDraftMenu');
		}
		else
		{
			Menu.StartMessage("Could not create a random companion. The roster may be full, or the network may be busy; try again.", 1);
		}
		return true;
	}
}

class CompanionBotsMenu : CompanionOptionMenu
{
	int mEnabledCount;
	int mCapacity;
	int mRosterCount;
	bool mCanManage;
	bool mCanDeploy;
	bool mCanBeginDraft;

	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		Super.Init(parent, desc);
		Build();
	}

	void Build()
	{
		mDesc.mItems.Clear();
		mEnabledCount = CompanionBots.GetEnabledCount();
		mCapacity = CompanionBots.GetCapacity();
		mRosterCount = CompanionBots.GetRosterCount();
		mCanManage = CompanionBots.CanManage();
		mCanDeploy = CompanionBots.CanDeploy();
		mCanBeginDraft = CompanionBots.CanBeginDraft();
		AddHeading(String.Format("Squad: %d configured", mEnabledCount));
		if (mCanDeploy)
			AddInfo(String.Format("Up to %d companions here", mCapacity));
		else
			AddInfo("Deathmatch never spawns companions.", "Changes are saved for co-op.");
		if (mEnabledCount == 0)
			AddInfo("Start at zero. Add one companion", "and configure it first.");
		else
			AddInfo("Select a companion to customize", "or remove that profile.");
		if (mCanDeploy && mRosterCount > 0 && mCapacity == 0)
			AddInfo("No co-op place is free here.", "New companions wait for a free place.");
		if (!mCanManage)
			AddInfo("Read-only: host or local controller", "can change the squad.");
		AddSpacer();

		for (int profile = 0; profile < 7; ++profile)
		{
			if (CompanionBots.IsProfileEnabled(profile))
				mDesc.mItems.Push(new("CompanionProfileLinkItem").Init(profile));
		}

		AddSpacer();
		if (mRosterCount == 0)
			AddInfo("No companion identities are available.", "Check bots.cfg or active game data.");
		else
		{
			mDesc.mItems.Push(new("CompanionBeginDraftItem").Init());
			mDesc.mItems.Push(new("CompanionBeginRandomDraftItem").Init());
			AddInfo("Random fills every profile choice;", "review it before adding.");
			if (mCanManage && !mCanBeginDraft)
				AddInfo("All usable identities or profiles", "are assigned. Remove one to add.");
		}
		mDesc.mItems.Push(new("OptionMenuItemSubmenu").Init("Browse identity library", 'CompanionRosterMenu'));
		if (CompanionBots.CanManage())
			mDesc.mItems.Push(new("OptionMenuItemOption").Init("Respawn companions after death", 'bot_companion_respawn', 'OnOff'));
		else
			AddInfo("Respawn setting is host-controlled.");

		if (mEnabledCount > 0 && mCanManage)
		{
			AddSpacer();
			mDesc.mItems.Push(new("OptionMenuItemSafeCommand").Init("Dismiss all companions", 'dismisscompanions', "This removes the active squad and clears its saved profiles."));
		}

		AddInfo("Works on ordinary and procedural maps.", "People and companions share 8 places.");
		mDesc.mSelectedItem = FirstSelectable();
		mDesc.mScrollPos = 0;
		mDesc.CalcIndent();
	}

	override void Ticker()
	{
		Super.Ticker();
		if (mEnabledCount != CompanionBots.GetEnabledCount() ||
			mCapacity != CompanionBots.GetCapacity() ||
			mRosterCount != CompanionBots.GetRosterCount() ||
			mCanManage != CompanionBots.CanManage() ||
			mCanDeploy != CompanionBots.CanDeploy() ||
			mCanBeginDraft != CompanionBots.CanBeginDraft())
			Build();
	}
}
