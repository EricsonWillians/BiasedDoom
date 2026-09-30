//=============================================================================
//
// Searchable preset picker for long OptionValues-backed preset lists.
//
//=============================================================================

class PresetMenuItem : OptionMenuItem
{
	PresetMenuItem Init(String label, Name cvarName, int presetValue)
	{
		Super.Init(label, cvarName, false);
		mCVar = CVar.FindCVar(cvarName);
		mPresetValue = presetValue;
		return self;
	}

	override int Draw(OptionMenuDescriptor desc, int y, int indent, bool selected)
	{
		bool isActive = mCVar != null && int(mCVar.GetFloat()) == mPresetValue;
		int labelColor = isActive
			? OptionMenuSettings.mFontColorHighlight
			: OptionMenuSettings.mFontColor;

		if (selected)
			labelColor = OptionMenuSettings.mFontColorSelection;

		drawLabel(indent, y, labelColor);

		if (isActive)
			drawValue(indent, y, OptionMenuSettings.mFontColorValue, "Current", false, false);

		return indent;
	}

	override bool Activate()
	{
		if (mCVar != null)
		{
			mCVar.SetFloat(mPresetValue);
			Menu.MenuSound("menu/choose");
		}
		return true;
	}

	private CVar mCVar;
	private int mPresetValue;
}

class PresetSearchField : OptionMenuItemTextField
{
	PresetSearchField Init(String label, PresetMenu menu, string query)
	{
		Super.Init(label, "");
		mMenu = menu;
		mText = query;
		return self;
	}

	override bool MenuEvent(int mkey, bool fromcontroller)
	{
		if (mkey == Menu.MKEY_Enter)
		{
			Menu.MenuSound("menu/choose");
			mEnter = TextEnterMenu.OpenTextEnter(Menu.GetCurrentMenu(), Menu.OptionFont(), mText, -1, fromcontroller);
			mEnter.ActivateMenu();
			return true;
		}
		if (mkey == Menu.MKEY_Input)
		{
			// MKEY_Input can arrive without a pending editor (e.g. after Search()
			// rebuilt the item list), so only read the text back when an editor
			// is actually open.
			if (mEnter != null)
			{
				mText = mEnter.GetText();
				mMenu.Search();
			}
			return true;
		}
		if (mkey == Menu.MKEY_Abort)
		{
			mEnter = null;
			return true;
		}

		return Super.MenuEvent(mkey, fromcontroller);
	}

	override String Represent()
	{
		return mEnter
			? mEnter.GetText() .. NewSmallFont.GetCursor()
			: mText;
	}

	String GetText() { return mText; }

	private PresetMenu mMenu;
	private string mText;
}

class PresetMenu : OptionMenu
{
	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		Super.Init(parent, desc);

		mDesc.mItems.Clear();
		AddSearchField("");
		Populate("");
	}

	void Search()
	{
		string query = mSearchField.GetText();

		mDesc.mItems.Clear();
		AddSearchField(query);
		Populate(query);

		mDesc.mScrollPos = 0;
		mDesc.mSelectedItem = 0;
	}

	virtual Name GetPresetCVarName()
	{
		return 'bd_graphics_preset';
	}

	virtual Name GetPresetValuesName()
	{
		return 'PostfxPresetModes';
	}

	// Hook for subclasses to annotate a row (e.g. layer pairing info).
	virtual String DecorateLabel(String label, int value)
	{
		return label;
	}

	private void AddSearchField(string query)
	{
		mSearchField = new("PresetSearchField").Init("Search presets", self, query);
		mDesc.mItems.Push(mSearchField);

		let separator = new("OptionMenuItemStaticText").Init("");
		mDesc.mItems.Push(separator);
	}

	private void Populate(string searchText)
	{
		let query = os_Query.FromString(searchText);
		Name valuesName = GetPresetValuesName();
		Name cvarName = GetPresetCVarName();
		int count = OptionValues.GetCount(valuesName);
		bool found = false;

		for (int i = 0; i < count; ++i)
		{
			string label = StringTable.Localize(OptionValues.GetText(valuesName, i));
			if (!query.Matches(label, false))
				continue;

			int value = int(OptionValues.GetValue(valuesName, i));
			let item = new("PresetMenuItem").Init(DecorateLabel(label, value), cvarName, value);
			mDesc.mItems.Push(item);
			found = true;
		}

		if (!found)
		{
			let noResults = new("OptionMenuItemStaticText").Init("No matching presets", 0);
			mDesc.mItems.Push(noResults);
		}

		mDesc.CalcIndent();
	}

	private PresetSearchField mSearchField;
}

class GraphicsPresetMenu : PresetMenu
{
	override Name GetPresetCVarName()
	{
		return 'bd_graphics_preset';
	}

	override Name GetPresetValuesName()
	{
		return 'PostfxPresetModes';
	}

	// Show which lighting/fog presets each graphics preset is paired with, so
	// the layer relationship is visible while browsing.
	override String DecorateLabel(String label, int value)
	{
		if (value <= 0)
			return label;
		int lighting = RenderingPresets.GetGraphicsPresetLighting(value);
		int fog = RenderingPresets.GetGraphicsPresetFog(value);
		if (lighting <= 0 && fog <= 0)
			return label;
		String lightingName = PresetLabelFor('BDLightingPresetModes', lighting);
		String fogName = PresetLabelFor('BDFogPresets', fog);
		if (lightingName == "" && fogName == "")
			return label;
		String pairing = lightingName;
		if (fogName != lightingName && fogName != "")
			pairing = pairing .. " / " .. fogName;
		return String.Format("%s  →  %s", label, pairing);
	}

	private static String PresetLabelFor(Name group, int value)
	{
		if (value <= 0)
			return "";
		for (int i = 0; i < OptionValues.GetCount(group); ++i)
		{
			if (int(OptionValues.GetValue(group, i)) == value)
				return StringTable.Localize(OptionValues.GetText(group, i));
		}
		return "";
	}
}

class LightingPresetMenu : PresetMenu
{
	override Name GetPresetCVarName()
	{
		return 'bd_lighting_preset';
	}

	override Name GetPresetValuesName()
	{
		return 'BDLightingPresetModes';
	}
}

class FogPresetMenu : PresetMenu
{
	override Name GetPresetCVarName()
	{
		return 'bd_fog_preset';
	}

	override Name GetPresetValuesName()
	{
		return 'BDFogPresets';
	}
}
