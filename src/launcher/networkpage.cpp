
#include "networkpage.h"
#include "launcherwindow.h"
#include "gstrings.h"
#include "c_cvars.h"
#include "i_net.h"
#include "i_interface.h"
#include <zwidget/core/resourcedata.h>
#include <zwidget/widgets/listview/listview.h>
#include <zwidget/widgets/textlabel/textlabel.h>
#include <zwidget/widgets/checkboxlabel/checkboxlabel.h>
#include <zwidget/widgets/lineedit/lineedit.h>
#include <zwidget/widgets/pushbutton/pushbutton.h>
#include <zwidget/widgets/tabwidget/tabwidget.h>
#include <zwidget/widgets/dropdown/dropdown.h>

#include <algorithm>
#include <cctype>

#ifdef BIASEDDOOM_ONLINE
#include "common/online/online_service.h"
#endif

EXTERN_CVAR(String, defaultonlineblocked);
EXTERN_CVAR(String, defaultonlinerecents);

constexpr double EditHeight = 24.0;

// Launcher fields are converted back into command-line arguments by the
// shared startup code. Quote values here so a session title such as
// "Friday co-op" remains one value and cannot introduce another switch.
static void AppendOnlineArgument(FString& arguments, const char* name, const std::string& value)
{
	if (value.empty())
		return;
	arguments.AppendFormat(" %s \"", name);
	for (const unsigned char character : value)
	{
		if (character < 32 || character == 127)
			continue;
		if (character == '\\' || character == '\"')
			arguments += '\\';
		arguments += static_cast<char>(character);
	}
	arguments += '\"';
}

static bool MatchesOnlineBrowseFilter(const std::string& value, const std::string& filter)
{
	if (filter.empty())
		return true;
	if (filter.size() > value.size())
		return false;
	return std::search(value.begin(), value.end(), filter.begin(), filter.end(), [](unsigned char left, unsigned char right)
	{
		return std::tolower(left) == std::tolower(right);
	}) != value.end();
}

static bool IsSafeOnlineJoinCode(const std::string& joinCode)
{
	if (joinCode.size() != 9 || joinCode[4] != '-')
		return false;
	for (size_t index = 0; index < joinCode.size(); ++index)
	{
		if (index == 4)
			continue;
		const unsigned char character = static_cast<unsigned char>(joinCode[index]);
		if (!(std::isdigit(character) || (character >= 'A' && character <= 'Z')))
			return false;
	}
	return true;
}

static bool IsSafeOnlineHistoryService(const std::string& serviceUrl)
{
	return !serviceUrl.empty() && serviceUrl.size() <= 255 &&
		serviceUrl.find_first_of("\r\n\t@") == std::string::npos;
}

static std::string DescribeOnlineBytes(uint64_t bytes)
{
	constexpr uint64_t KiB = 1024;
	constexpr uint64_t MiB = KiB * KiB;
	constexpr uint64_t GiB = MiB * KiB;
	const auto describe = [bytes](uint64_t unit, const char* suffix)
	{
		const uint64_t tenths = (bytes * 10 + unit / 2) / unit;
		return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " " + suffix;
	};
	if (bytes >= GiB)
		return describe(GiB, "GiB");
	if (bytes >= MiB)
		return describe(MiB, "MiB");
	if (bytes >= KiB)
		return describe(KiB, "KiB");
	return std::to_string(bytes) + " bytes";
}

NetworkPage::NetworkPage(LauncherWindow* launcher, const FStartupSelectionInfo& info) : Widget(nullptr), Launcher(launcher)
{
	ParametersEdit = new LineEdit(this);
	ParametersLabel = new TextLabel(this);
	SaveFileEdit = new LineEdit(this);
	SaveFileLabel = new TextLabel(this);
	SaveFileCheckbox = new CheckboxLabel(this);
	SaveParametersCheckbox = new CheckboxLabel(this);
	IWADsDropdown = new Dropdown(this);

	SaveFileCheckbox->SetChecked(info.bSaveNetFile);
	if (!info.DefaultNetSaveFile.IsEmpty())
		SaveFileEdit->SetText(info.DefaultNetSaveFile.GetChars());

	SaveParametersCheckbox->SetChecked(info.bSaveNetArgs);
	if (!info.DefaultNetArgs.IsEmpty())
		ParametersEdit->SetText(info.DefaultNetArgs.GetChars());

	StartPages = new TabWidget(this);
	HostPage = new HostSubPage(this, info);
	JoinPage = new JoinSubPage(this, info);
#ifdef BIASEDDOOM_ONLINE
	BrowsePage = new BrowseSubPage(this, info);
#endif

	for (const auto& wad : *info.Wads)
	{
		const char* filepart = strrchr(wad.Path.GetChars(), '/');
		if (filepart == nullptr)
			filepart = wad.Path.GetChars();
		else
			++filepart;

		FString work;
		if (*filepart)
			work.Format("%s (%s)", wad.Name.GetChars(), filepart);
		else
			work = wad.Name.GetChars();

		IWADsDropdown->AddItem(work.GetChars());
	}

	if (info.DefaultNetIWAD >= 0 && info.DefaultNetIWAD < info.Wads->SSize())
		IWADsDropdown->SetSelectedItem(info.DefaultNetIWAD);
}

// This has to be done after the main page is parented, otherwise it won't have the correct
// info to pull from.
void NetworkPage::InitializeTabs(const FStartupSelectionInfo& info)
{
	StartPages->AddTab(HostPage, "Host");
	StartPages->AddTab(JoinPage, "Join");
#ifdef BIASEDDOOM_ONLINE
	StartPages->AddTab(BrowsePage, "Browse");
#endif

	switch (info.DefaultNetPage)
	{
	case 1:
		StartPages->SetCurrentWidget(JoinPage);
		break;
	default:
		StartPages->SetCurrentWidget(HostPage);
		break;
	}
}

void NetworkPage::SelectOnlineSession(const FString& serviceUrl, const FString& joinCode)
{
	JoinPage->SetOnlineSession(serviceUrl, joinCode);
	StartPages->SetCurrentWidget(JoinPage);
	UpdatePlayButton();
}

void NetworkPage::OnIWADsListActivated()
{
	Launcher->Start();
}

void NetworkPage::OnSetFocus()
{
	IWADsDropdown->SetFocus();
}

void NetworkPage::SetValues(FStartupSelectionInfo& info) const
{
	info.DefaultNetIWAD = IWADsDropdown->GetSelectedItem();
	info.DefaultNetArgs = ParametersEdit->GetText();

	info.bHosting = IsInHost();
	if (info.bHosting)
	{
		info.DefaultNetPage = 0;
		HostPage->SetValues(info);
	}
	else
	{
		info.DefaultNetPage = 1;
		JoinPage->SetValues(info);
	}

	info.bSaveNetFile = SaveFileCheckbox->GetChecked();
	info.bSaveNetArgs = SaveParametersCheckbox->GetChecked();
	const auto save = SaveFileEdit->GetText();
	if (!save.empty())
		info.AdditionalNetArgs.AppendFormat(" -loadgame %s", save.c_str());
	info.DefaultNetSaveFile = save;
}

void NetworkPage::UpdatePlayButton()
{
	Launcher->UpdatePlayButton();
}

bool NetworkPage::IsInHost() const
{
	return StartPages->GetCurrentIndex() >= 0 ? StartPages->GetCurrentWidget() == HostPage : false;
}

void NetworkPage::OnGeometryChanged()
{
	const double w = GetWidth();
	const double h = GetHeight();

	double y = 0.0;
	IWADsDropdown->SetFrameGeometry(0.0, y, w, IWADsDropdown->GetPreferredHeight());
	y += IWADsDropdown->GetPreferredHeight() + 7.0;
	const double pageTop = y;

	y = h - SaveFileCheckbox->GetPreferredHeight();
	SaveFileCheckbox->SetFrameGeometry(0.0, y, w, SaveFileCheckbox->GetPreferredHeight());

	y -= SaveParametersCheckbox->GetPreferredHeight();
	SaveParametersCheckbox->SetFrameGeometry(0.0, y, w, SaveParametersCheckbox->GetPreferredHeight());

	y -= EditHeight + 2.0;
	ParametersEdit->SetFrameGeometry(0.0, y, w, EditHeight);
	y -= ParametersLabel->GetPreferredHeight();
	ParametersLabel->SetFrameGeometry(0.0, y, w, ParametersLabel->GetPreferredHeight());

	const double wSize = w * 0.5;

	y -= EditHeight + 2.0;
	SaveFileEdit->SetFrameGeometry(0.0, y, wSize, EditHeight);
	y -= SaveFileLabel->GetPreferredHeight();
	SaveFileLabel->SetFrameGeometry(0.0, y, wSize, SaveFileLabel->GetPreferredHeight());

	StartPages->SetFrameGeometry(0.0, pageTop, w, y - pageTop);
}

void NetworkPage::UpdateLanguage()
{
	ParametersLabel->SetText(GStrings.GetString("PICKER_ADDPARM"));
	SaveFileLabel->SetText(GStrings.GetString("PICKER_LOADSAVE"));
	SaveFileCheckbox->SetText(GStrings.GetString("PICKER_REMSAVE"));
	SaveParametersCheckbox->SetText(GStrings.GetString("PICKER_REMPARM"));

	StartPages->SetTabText(HostPage, GStrings.GetString("PICKER_HOST"));
	StartPages->SetTabText(JoinPage, GStrings.GetString("PICKER_JOIN"));
	if (BrowsePage != nullptr)
		StartPages->SetTabText(BrowsePage, "Browse");
	HostPage->UpdateLanguage();
	JoinPage->UpdateLanguage();
	if (BrowsePage != nullptr)
		BrowsePage->UpdateLanguage();
}

HostSubPage::HostSubPage(NetworkPage* main, const FStartupSelectionInfo& info) : Widget(nullptr), MainTab(main)
{
	NetModesLabel = new TextLabel(this);
	NetModesDropdown = new Dropdown(this);

	NetModesDropdown->AddItem("Auto (recommended)");
	NetModesDropdown->AddItem("Packet-Server");
	NetModesDropdown->AddItem("Peer-to-Peer");
	NetModesDropdown->SetSelectedItem(max<int>(info.DefaultNetMode, 0));

	TicDupLabel = new TextLabel(this);
	TicDupDropdown = new Dropdown(this);
	ExtraTicCheckbox = new CheckboxLabel(this);

	TicDupDropdown->AddItem("35 Hz");
	TicDupDropdown->AddItem("17.5 Hz");
	TicDupDropdown->AddItem("11.6 Hz");
	TicDupDropdown->SetSelectedItem(max<int>(info.DefaultNetTicDup, 0));

	ExtraTicCheckbox->SetChecked(info.DefaultNetExtraTic);

	GameModesLabel = new TextLabel(this);
	GameModesDropdown = new Dropdown(this);
	AltDeathmatchCheckbox = new CheckboxLabel(this);

	GameModesDropdown->AddItem("None");
	GameModesDropdown->AddItem("Co-op");
	GameModesDropdown->AddItem("Deathmatch");
	GameModesDropdown->AddItem("Team Deathmatch");
	GameModesDropdown->SetSelectedItem(max<int>(info.DefaultNetGameMode, 0));

	TeamLabel = new TextLabel(this);
	TeamEdit = new LineEdit(this);

	AltDeathmatchCheckbox->SetChecked(info.DefaultNetAltDM);

	TeamEdit->SetMaxLength(3);
	TeamEdit->SetNumericMode(true);
	TeamEdit->SetTextInt(info.DefaultNetHostTeam);

	MaxPlayersEdit = new LineEdit(this);
	PortEdit = new LineEdit(this);
	MaxPlayersLabel = new TextLabel(this);
	PortLabel = new TextLabel(this);

	MaxPlayersEdit->SetMaxLength(2);
	MaxPlayersEdit->SetNumericMode(true);
	MaxPlayersEdit->SetTextInt(info.DefaultNetPlayers);
	PortEdit->SetMaxLength(5);
	PortEdit->SetNumericMode(true);
	if (info.DefaultNetHostPort > 0)
		PortEdit->SetTextInt(info.DefaultNetHostPort);

	MaxPlayerHintLabel = new TextLabel(this);
	PortHintLabel = new TextLabel(this);
	TeamHintLabel = new TextLabel(this);

	OnlineCheckbox = new CheckboxLabel(this);
	OnlineCheckbox->SetChecked(info.DefaultNetOnline);
	OnlineConnectivityHint = new TextLabel(this);
	OnlineServiceLabel = new TextLabel(this);
	OnlineServiceEdit = new LineEdit(this);
	OnlineServiceEdit->SetText(info.DefaultOnlineService.GetChars());
	OnlineServiceEdit->SetMaxLength(255);
	OnlineTitleLabel = new TextLabel(this);
	OnlineTitleEdit = new LineEdit(this);
	OnlineTitleEdit->SetText(info.DefaultOnlineTitle.GetChars());
	OnlineTitleEdit->SetMaxLength(96);
	OnlineRegionLabel = new TextLabel(this);
	OnlineRegionEdit = new LineEdit(this);
	OnlineRegionEdit->SetText(info.DefaultOnlineRegion.GetChars());
	OnlineRegionEdit->SetMaxLength(32);
	OnlinePasswordLabel = new TextLabel(this);
	OnlinePasswordEdit = new LineEdit(this);
	OnlinePasswordEdit->SetMaxLength(255);
	OnlinePasswordEdit->SetPasswordMode();
	OnlinePublicCheckbox = new CheckboxLabel(this);
	OnlinePublicCheckbox->SetChecked(info.DefaultOnlinePublic);
	OnlineModSummary = new TextLabel(this);

#ifndef BIASEDDOOM_ONLINE
	// Do not let a feature-reduced build create launch arguments that the
	// engine must immediately reject. Keeping the controls visible but disabled
	// explains why the online path is unavailable while preserving the normal
	// direct-IP/LAN host controls beside it.
	OnlineCheckbox->SetChecked(false);
	OnlineCheckbox->SetDisabled(true);
	OnlineConnectivityHint->SetDisabled(true);
	OnlineServiceLabel->SetDisabled(true);
	OnlineServiceEdit->SetDisabled(true);
	OnlineTitleLabel->SetDisabled(true);
	OnlineTitleEdit->SetDisabled(true);
	OnlineRegionLabel->SetDisabled(true);
	OnlineRegionEdit->SetDisabled(true);
	OnlinePasswordLabel->SetDisabled(true);
	OnlinePasswordEdit->SetDisabled(true);
	OnlinePublicCheckbox->SetDisabled(true);
	OnlineModSummary->SetDisabled(true);
#endif

	// TODO: Themeable de-emphasis
	// MaxPlayerHintLabel->SetStyleColor("color", Colorf::fromRgba8(160, 160, 160));
	// PortHintLabel->SetStyleColor("color", Colorf::fromRgba8(160, 160, 160));
	// TeamHintLabel->SetStyleColor("color", Colorf::fromRgba8(160, 160, 160));
}

void HostSubPage::SetValues(FStartupSelectionInfo& info) const
{
	info.AdditionalNetArgs = "";
	info.DefaultNetMode = NetModesDropdown->GetSelectedItem();
	switch (info.DefaultNetMode)
	{
	case 1:
		info.AdditionalNetArgs.AppendFormat(" -netmode 1");
		break;
	case 2:
		info.AdditionalNetArgs.AppendFormat(" -netmode 0");
		break;
	}

	info.DefaultNetExtraTic = ExtraTicCheckbox->GetChecked();
	if (info.DefaultNetExtraTic)
		info.AdditionalNetArgs.AppendFormat(" -extratic");

	const int dup = TicDupDropdown->GetSelectedItem();
	if (dup > 0)
		info.AdditionalNetArgs.AppendFormat(" -dup %d", dup + 1);
	info.DefaultNetTicDup = dup;

	info.DefaultNetPlayers = clamp<int>(MaxPlayersEdit->GetTextInt(), 1, MAXPLAYERS);
	info.DefaultNetOnline = OnlineCheckbox->GetChecked();
	info.DefaultOnlineService = OnlineServiceEdit->GetText();
	info.DefaultOnlineTitle = OnlineTitleEdit->GetText();
	info.DefaultOnlineRegion = OnlineRegionEdit->GetText();
	info.DefaultOnlinePublic = OnlinePublicCheckbox->GetChecked();
	if (info.DefaultNetOnline)
	{
		const char* onlineMode = "standard";
		switch (GameModesDropdown->GetSelectedItem())
		{
		case 1: onlineMode = "co-op"; break;
		case 2: onlineMode = "deathmatch"; break;
		case 3: onlineMode = "team deathmatch"; break;
		default: break;
		}
		info.AdditionalNetArgs.AppendFormat(" -online-host %d", info.DefaultNetPlayers);
		AppendOnlineArgument(info.AdditionalNetArgs, "-online-service", info.DefaultOnlineService.GetChars());
		AppendOnlineArgument(info.AdditionalNetArgs, "-online-title", info.DefaultOnlineTitle.GetChars());
		AppendOnlineArgument(info.AdditionalNetArgs, "-online-region", info.DefaultOnlineRegion.GetChars());
		AppendOnlineArgument(info.AdditionalNetArgs, "-online-mode", onlineMode);
		AppendOnlineArgument(info.AdditionalNetArgs, "-password", OnlinePasswordEdit->GetText());
		if (info.DefaultOnlinePublic)
			info.AdditionalNetArgs.AppendFormat(" -online-public");
	}
	else
	{
		info.AdditionalNetArgs.AppendFormat(" -host %d", info.DefaultNetPlayers);
	}
	const int port = clamp<int>(PortEdit->GetTextInt(), 0, UINT16_MAX);
	if (port > 0)
	{
		info.AdditionalNetArgs.AppendFormat(" -port %d", port);
		info.DefaultNetHostPort = port;
	}
	else
	{
		info.DefaultNetHostPort = 0;
	}

	info.DefaultNetAltDM = AltDeathmatchCheckbox->GetChecked();
	info.DefaultNetGameMode = GameModesDropdown->GetSelectedItem();
	switch (info.DefaultNetGameMode)
	{
	case 1:
		info.AdditionalNetArgs.AppendFormat(" -coop");
		break;
	case 3:
		{
			info.AdditionalNetArgs.AppendFormat(" +teamplay 1");
			int team = 255;
			if (!TeamEdit->GetText().empty())
			{
				team = TeamEdit->GetTextInt();
				if (team < 0 || team > 255)
					team = 255;
			}
			info.AdditionalNetArgs.AppendFormat(" +team %d", team);
			info.DefaultNetHostTeam = team;
		}
		[[fallthrough]];
	case 2:
		if (AltDeathmatchCheckbox->GetChecked())
			info.AdditionalNetArgs.AppendFormat(" -altdeath");
		else
			info.AdditionalNetArgs.AppendFormat(" -deathmatch");
		break;
	}
}

void HostSubPage::UpdateLanguage()
{
	NetModesLabel->SetText(GStrings.GetString("PICKER_NETMODE"));
	NetModesDropdown->UpdateItem(GStrings.GetString("PICKER_NETAUTO"), 0);
	NetModesDropdown->UpdateItem(GStrings.GetString("PICKER_NETSERVER"), 1);
	NetModesDropdown->UpdateItem(GStrings.GetString("PICKER_NETPEER"), 2);

	TicDupLabel->SetText(GStrings.GetString("PICKER_NETRATE"));
	ExtraTicCheckbox->SetText(GStrings.GetString("PICKER_NETBACKUP"));

	GameModesLabel->SetText(GStrings.GetString("PICKER_GAMEMODE"));
	GameModesDropdown->UpdateItem(GStrings.GetString("OPTVAL_NONE"), 0);
	GameModesDropdown->UpdateItem(GStrings.GetString("PICKER_COOP"), 1);
	GameModesDropdown->UpdateItem(GStrings.GetString("PICKER_DM"), 2);
	GameModesDropdown->UpdateItem(GStrings.GetString("PICKER_TDM"), 3);
	AltDeathmatchCheckbox->SetText(GStrings.GetString("PICKER_ALTDM"));
	TeamLabel->SetText(GStrings.GetString("PICKER_TEAM"));

	MaxPlayersLabel->SetText(GStrings.GetString("PICKER_PLAYERS"));
	PortLabel->SetText(GStrings.GetString("PICKER_PORT"));

	MaxPlayerHintLabel->SetText(GStrings.GetString("PICKER_PLAYERHINT"));
	PortHintLabel->SetText(GStrings.GetString("PICKER_PORTHINT"));
	TeamHintLabel->SetText(GStrings.GetString("PICKER_TEAMHINT"));
#ifdef BIASEDDOOM_ONLINE
	OnlineCheckbox->SetText("Use Online Relay (supports joining games in progress)");
	OnlineConnectivityHint->SetText("Relay is automatic; a verified direct route is used when available. No port forwarding.");
	#else
	OnlineCheckbox->SetText("Online Relay is unavailable in this build");
	OnlineConnectivityHint->SetText("Build with online support to host through the relay service.");
	#endif
	OnlineServiceLabel->SetText("Online service URL");
	OnlineTitleLabel->SetText("Session title");
	OnlineRegionLabel->SetText("Region");
	OnlinePasswordLabel->SetText("Session password (not saved)");
	OnlinePublicCheckbox->SetText("List this session publicly");
	OnlineModSummary->SetText("Mod sharing: guests review host mods before encrypted download and mounting.");
}

void HostSubPage::OnGeometryChanged()
{
	const double w = GetWidth();
	const double h = GetHeight();

	constexpr double LabelOfsSize = 100.0;
	constexpr double hintOfs = 160.0;
	constexpr double DropdownSize = 220.0;
	constexpr double EditWidth = 55.0;

	double y = 0.0;
	const double wSize = w * 0.5;

	MaxPlayersLabel->SetFrameGeometry(0.0, y, LabelOfsSize, MaxPlayersLabel->GetPreferredHeight());
	MaxPlayersEdit->SetFrameGeometry(LabelOfsSize, y, EditWidth, EditHeight);
	y += EditHeight + 2.0;

	PortLabel->SetFrameGeometry(0.0, y, LabelOfsSize, PortLabel->GetPreferredHeight());
	PortEdit->SetFrameGeometry(LabelOfsSize, y, EditWidth, EditHeight);

	MaxPlayerHintLabel->SetFrameGeometry(hintOfs, 0.0, wSize - hintOfs, MaxPlayerHintLabel->GetPreferredHeight());
	PortHintLabel->SetFrameGeometry(hintOfs, y, wSize - hintOfs, PortHintLabel->GetPreferredHeight());

	y += EditHeight + 7.0;

	GameModesLabel->SetFrameGeometry(0.0, y, wSize, GameModesLabel->GetPreferredHeight());
	y += GameModesLabel->GetPreferredHeight();
	GameModesDropdown->SetFrameGeometry(0.0, y, DropdownSize, GameModesDropdown->GetPreferredHeight());
	y += GameModesDropdown->GetPreferredHeight() + 2.0;

	TeamLabel->SetFrameGeometry(0.0, y, LabelOfsSize, TeamLabel->GetPreferredHeight());
	TeamEdit->SetFrameGeometry(LabelOfsSize, y, EditWidth, EditHeight);
	TeamHintLabel->SetFrameGeometry(hintOfs, y, wSize - hintOfs, TeamHintLabel->GetPreferredHeight());
	y += EditHeight + 2.0;

	AltDeathmatchCheckbox->SetFrameGeometry(0.0, y, wSize, AltDeathmatchCheckbox->GetPreferredHeight());

	y = 0.0;
	
	TicDupLabel->SetFrameGeometry(w - DropdownSize, y, DropdownSize, TicDupLabel->GetPreferredHeight());
	y += TicDupLabel->GetPreferredHeight();
	TicDupDropdown->SetFrameGeometry(w - DropdownSize, y, DropdownSize, TicDupDropdown->GetPreferredHeight());
	y += TicDupDropdown->GetPreferredHeight() + 2.0;

	NetModesLabel->SetFrameGeometry(w - DropdownSize, y, DropdownSize, NetModesLabel->GetPreferredHeight());
	y += NetModesLabel->GetPreferredHeight();
	NetModesDropdown->SetFrameGeometry(w - DropdownSize, y, DropdownSize, NetModesDropdown->GetPreferredHeight());
	y += NetModesDropdown->GetPreferredHeight() + 2.0;

	ExtraTicCheckbox->SetFrameGeometry(w - DropdownSize, y, DropdownSize, ExtraTicCheckbox->GetPreferredHeight());

	y += ExtraTicCheckbox->GetPreferredHeight() + 12.0;
	OnlineCheckbox->SetFrameGeometry(w - DropdownSize, y, DropdownSize, OnlineCheckbox->GetPreferredHeight());
	y += OnlineCheckbox->GetPreferredHeight() + 2.0;
	OnlineConnectivityHint->SetFrameGeometry(w - DropdownSize, y, DropdownSize, OnlineConnectivityHint->GetPreferredHeight());
	y += OnlineConnectivityHint->GetPreferredHeight() + 2.0;
	OnlineServiceLabel->SetFrameGeometry(w - DropdownSize, y, DropdownSize, OnlineServiceLabel->GetPreferredHeight());
	y += OnlineServiceLabel->GetPreferredHeight();
	OnlineServiceEdit->SetFrameGeometry(w - DropdownSize, y, DropdownSize, EditHeight);
	y += EditHeight + 2.0;
	OnlineTitleLabel->SetFrameGeometry(w - DropdownSize, y, DropdownSize, OnlineTitleLabel->GetPreferredHeight());
	y += OnlineTitleLabel->GetPreferredHeight();
	OnlineTitleEdit->SetFrameGeometry(w - DropdownSize, y, DropdownSize, EditHeight);
	y += EditHeight + 2.0;
	OnlineRegionLabel->SetFrameGeometry(w - DropdownSize, y, DropdownSize, OnlineRegionLabel->GetPreferredHeight());
	y += OnlineRegionLabel->GetPreferredHeight();
	OnlineRegionEdit->SetFrameGeometry(w - DropdownSize, y, DropdownSize, EditHeight);
	y += EditHeight + 2.0;
	OnlinePasswordLabel->SetFrameGeometry(w - DropdownSize, y, DropdownSize, OnlinePasswordLabel->GetPreferredHeight());
	y += OnlinePasswordLabel->GetPreferredHeight();
	OnlinePasswordEdit->SetFrameGeometry(w - DropdownSize, y, DropdownSize, EditHeight);
	y += EditHeight + 2.0;
	OnlinePublicCheckbox->SetFrameGeometry(w - DropdownSize, y, DropdownSize, OnlinePublicCheckbox->GetPreferredHeight());
	y += OnlinePublicCheckbox->GetPreferredHeight() + 5.0;
	OnlineModSummary->SetFrameGeometry(0.0, y, w, OnlineModSummary->GetPreferredHeight());

	MainTab->UpdatePlayButton();
}

JoinSubPage::JoinSubPage(NetworkPage* main, const FStartupSelectionInfo& info) : Widget(nullptr), MainTab(main)
{
	AddressEdit = new LineEdit(this);
	AddressPortEdit = new LineEdit(this);
	AddressLabel = new TextLabel(this);
	AddressPortLabel = new TextLabel(this);

	AddressEdit->SetText(info.DefaultNetAddress.GetChars());
	AddressPortEdit->SetMaxLength(5);
	AddressPortEdit->SetNumericMode(true);
	if (info.DefaultNetJoinPort > 0)
		AddressPortEdit->SetTextInt(info.DefaultNetJoinPort);

	TeamDeathmatchLabel = new TextLabel(this);
	TeamLabel = new TextLabel(this);
	TeamEdit = new LineEdit(this);

	TeamEdit->SetMaxLength(3);
	TeamEdit->SetNumericMode(true);
	TeamEdit->SetTextInt(info.DefaultNetJoinTeam);

	AddressPortHintLabel = new TextLabel(this);
	TeamHintLabel = new TextLabel(this);
	OnlineCheckbox = new CheckboxLabel(this);
	OnlineCheckbox->SetChecked(info.DefaultNetOnline);
	OnlineConnectivityHint = new TextLabel(this);
	OnlineServiceLabel = new TextLabel(this);
	OnlineServiceEdit = new LineEdit(this);
	OnlineServiceEdit->SetText(info.DefaultOnlineService.GetChars());
	OnlineServiceEdit->SetMaxLength(255);
	OnlineCodeLabel = new TextLabel(this);
	OnlineCodeEdit = new LineEdit(this);
	OnlineCodeEdit->SetText(info.DefaultNetAddress.GetChars());
	OnlineCodeEdit->SetMaxLength(9);
	OnlinePasswordLabel = new TextLabel(this);
	OnlinePasswordEdit = new LineEdit(this);
	OnlinePasswordEdit->SetMaxLength(255);
	OnlinePasswordEdit->SetPasswordMode();
	OnlineReviewButton = new PushButton(this);
	OnlineReviewButton->OnClick = [this]() { ReviewOnlineContent(); };
	OnlineReviewLabel = new TextLabel(this);
	OnlineReviewList = new ListView(this);
	OnlineReviewList->SetColumnWidths({ 0.34, 0.16, 0.50 });
	OnlineReviewList->SetHidden(true);
	OnlineAcceptModsCheckbox = new CheckboxLabel(this);
	OnlineAcceptModsCheckbox->SetChecked(false);
	OnlineAcceptModsCheckbox->SetDisabled(true);
	OnlineServiceEdit->FuncAfterEditChanged = [this]() { ResetOnlineReview(); };
	OnlineCodeEdit->FuncAfterEditChanged = [this]() { ResetOnlineReview(); };
	OnlinePasswordEdit->FuncAfterEditChanged = [this]() { ResetOnlineReview(); };
	OnlineRecentLabel = new TextLabel(this);
	OnlineRecentDropdown = new Dropdown(this);
	OnlineRecentDropdown->AddItem("Recent sessions...");
	const std::string storedRecents = static_cast<const char*>(defaultonlinerecents);
	for (size_t start = 0; start < storedRecents.size() && RecentSessions.Size() < 10; )
	{
		const size_t end = storedRecents.find('\n', start);
		const auto record = storedRecents.substr(start, end == std::string::npos ? std::string::npos : end - start);
		const size_t separator = record.find('\t');
		if (separator != std::string::npos && record.find('\t', separator + 1) == std::string::npos)
		{
			const auto service = record.substr(0, separator);
			const auto joinCode = record.substr(separator + 1);
			if (IsSafeOnlineHistoryService(service) && IsSafeOnlineJoinCode(joinCode))
				RecentSessions.Push({ service.c_str(), joinCode.c_str() });
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	for (const auto& recent : RecentSessions)
		OnlineRecentDropdown->AddItem((std::string(recent.JoinCode.GetChars()) + " - " + recent.ServiceUrl.GetChars()));
	OnlineRecentDropdown->SetSelectedItem(0);
	OnlineRecentDropdown->OnChanged = [this](int selection) { OnRecentSessionChanged(selection); };

#ifndef BIASEDDOOM_ONLINE
	OnlineCheckbox->SetChecked(false);
	OnlineCheckbox->SetDisabled(true);
	OnlineConnectivityHint->SetDisabled(true);
	OnlineServiceLabel->SetDisabled(true);
	OnlineServiceEdit->SetDisabled(true);
	OnlineCodeLabel->SetDisabled(true);
	OnlineCodeEdit->SetDisabled(true);
	OnlinePasswordLabel->SetDisabled(true);
	OnlinePasswordEdit->SetDisabled(true);
	OnlineReviewButton->SetDisabled(true);
	OnlineReviewLabel->SetDisabled(true);
	OnlineReviewList->SetDisabled(true);
	OnlineAcceptModsCheckbox->SetDisabled(true);
	OnlineRecentLabel->SetDisabled(true);
	OnlineRecentDropdown->SetDisabled(true);
#endif

	// TODO: Themeable de-emphasis
	// AddressPortHintLabel->SetStyleColor("color", Colorf::fromRgba8(160, 160, 160));
	// TeamHintLabel->SetStyleColor("color", Colorf::fromRgba8(160, 160, 160));
}

void JoinSubPage::SetValues(FStartupSelectionInfo& info)
{
	FString addr = AddressEdit->GetText();
	info.DefaultNetOnline = OnlineCheckbox->GetChecked();
	info.DefaultOnlineService = OnlineServiceEdit->GetText();
	// The controls above are disabled in an offline build, but keep this
	// branch out of that configuration as well: approval tokens are supplied
	// by the optional BdOnline client library.
#ifdef BIASEDDOOM_ONLINE
	if (info.DefaultNetOnline)
	{
		info.DefaultNetAddress = OnlineCodeEdit->GetText();
		info.AdditionalNetArgs = "";
		RememberOnlineSession();
		AppendOnlineArgument(info.AdditionalNetArgs, "-online-service", info.DefaultOnlineService.GetChars());
		AppendOnlineArgument(info.AdditionalNetArgs, "-online-join", info.DefaultNetAddress.GetChars());
		AppendOnlineArgument(info.AdditionalNetArgs, "-password", OnlinePasswordEdit->GetText());
		if (OnlineAcceptModsCheckbox->GetChecked() && HasCurrentOnlineReview() &&
			!ReviewedContentRootDigest.IsEmpty() && !ReviewedHostPeerId.IsEmpty())
		{
			std::string approval;
			std::string approvalError;
			if (BdOnline::OnlineBuildContentApprovalToken(ReviewedContentRootDigest.GetChars(), ReviewedHostPeerId.GetChars(), approval, approvalError))
				AppendOnlineArgument(info.AdditionalNetArgs, "-online-accept-mods", approval);
		}
		return;
	}
#else
	info.DefaultNetOnline = false;
#endif
	info.DefaultNetAddress = addr;
	const int port = clamp<int>(AddressPortEdit->GetTextInt(), 0, UINT16_MAX);
	if (port > 0)
	{
		addr.AppendFormat(":%d", port);
		info.DefaultNetJoinPort = port;
	}
	else
	{
		info.DefaultNetJoinPort = 0;
	}

	info.AdditionalNetArgs = "";
	info.AdditionalNetArgs.AppendFormat(" -join %s", addr.GetChars());

	int team = 255;
	if (!TeamEdit->GetText().empty())
	{
		team = TeamEdit->GetTextInt();
		if (team < 0 || team > 255)
			team = 255;
	}
	info.AdditionalNetArgs.AppendFormat(" +team %d", team);
	info.DefaultNetJoinTeam = team;
}

void JoinSubPage::SetOnlineSession(const FString& serviceUrl, const FString& joinCode)
{
	OnlineCheckbox->SetChecked(true);
	OnlineServiceEdit->SetText(serviceUrl.GetChars());
	OnlineCodeEdit->SetText(joinCode.GetChars());
	ResetOnlineReview();
}

void JoinSubPage::OnRecentSessionChanged(int selection)
{
	if (selection <= 0 || selection > static_cast<int>(RecentSessions.Size()))
		return;
	const auto& recent = RecentSessions[selection - 1];
	OnlineCheckbox->SetChecked(true);
	OnlineServiceEdit->SetText(recent.ServiceUrl.GetChars());
	OnlineCodeEdit->SetText(recent.JoinCode.GetChars());
	ResetOnlineReview();
	MainTab->UpdatePlayButton();
}

bool JoinSubPage::HasCurrentOnlineReview() const
{
	return OnlineReviewComplete && ReviewedOnlineRevision == OnlineReviewRevision;
}

void JoinSubPage::ResetOnlineReview()
{
	++OnlineReviewRevision;
	OnlineReviewComplete = false;
	ReviewedContentRootDigest = "";
	ReviewedHostPeerId = "";
	OnlineAcceptModsCheckbox->SetChecked(false);
	OnlineAcceptModsCheckbox->SetDisabled(true);
	OnlineReviewLabel->SetText("Review the host's mod plan before download approval is available.");
	OnlineReviewList->SetHidden(true);
	OnGeometryChanged();
}

void JoinSubPage::ReviewOnlineContent()
{
#ifdef BIASEDDOOM_ONLINE
	FOnlineJoinReview review;
	std::string error;
	const std::string serviceUrl = OnlineServiceEdit->GetText();
	const std::string joinCode = OnlineCodeEdit->GetText();
	const std::string password = OnlinePasswordEdit->GetText();
	if (!I_OnlineInspectJoin(serviceUrl, joinCode, password, review, error))
	{
		OnlineAcceptModsCheckbox->SetChecked(false);
		OnlineAcceptModsCheckbox->SetDisabled(true);
		OnlineReviewList->SetHidden(true);
		OnlineReviewLabel->SetText(("Could not review host mods: " + error).c_str());
		OnGeometryChanged();
		return;
	}
	while (OnlineReviewList->GetItemAmount() != 0)
		OnlineReviewList->RemoveItem(0);
	for (const auto& package : review.Packages)
	{
		OnlineReviewList->AddItem(package.DisplayName);
		const int row = static_cast<int>(OnlineReviewList->GetItemAmount() - 1);
		OnlineReviewList->UpdateItem(DescribeOnlineBytes(package.Size), row, 1);
		OnlineReviewList->UpdateItem(package.Digest, row, 2);
	}
	OnlineReviewList->SetHidden(false);
	ReviewedOnlineRevision = OnlineReviewRevision;
	OnlineReviewComplete = true;
	ReviewedContentRootDigest = review.ContentRootDigest.c_str();
	ReviewedHostPeerId = review.HostPeerId.c_str();
	OnlineAcceptModsCheckbox->SetChecked(false);
	OnlineAcceptModsCheckbox->SetDisabled(false);
	const std::string summary = "Host ID " + review.HostPeerId + " · " +
		std::to_string(review.Packages.size()) + " package(s) · " + DescribeOnlineBytes(review.TotalBytes) +
		". Review complete; approval applies only to this exact content plan.";
	OnlineReviewLabel->SetText(summary.c_str());
	OnGeometryChanged();
#else
	OnlineReviewLabel->SetText("This build was made without online multiplayer support.");
#endif
}

void JoinSubPage::RememberOnlineSession()
{
	const std::string service = OnlineServiceEdit->GetText();
	const std::string joinCode = OnlineCodeEdit->GetText();
	if (!IsSafeOnlineHistoryService(service) || !IsSafeOnlineJoinCode(joinCode))
		return;
	for (int index = RecentSessions.Size() - 1; index >= 0; --index)
	{
		if (!RecentSessions[index].ServiceUrl.CompareNoCase(service.c_str()) && !RecentSessions[index].JoinCode.CompareNoCase(joinCode.c_str()))
			RecentSessions.Delete(index);
	}
	RecentSessions.Insert(0, { service.c_str(), joinCode.c_str() });
	while (RecentSessions.Size() > 10)
		RecentSessions.Delete(RecentSessions.Size() - 1);
	SaveRecentSessions();
}

void JoinSubPage::SaveRecentSessions() const
{
	std::string encoded;
	for (const auto& recent : RecentSessions)
	{
		if (!encoded.empty())
			encoded += '\n';
		encoded += recent.ServiceUrl.GetChars();
		encoded += '\t';
		encoded += recent.JoinCode.GetChars();
	}
	defaultonlinerecents = encoded.c_str();
}

void JoinSubPage::UpdateLanguage()
{
	AddressLabel->SetText(GStrings.GetString("PICKER_IP"));
	AddressPortLabel->SetText(GStrings.GetString("PICKER_PORT"));

	TeamDeathmatchLabel->SetText(GStrings.GetString("PICKER_TDMLABEL"));
	TeamLabel->SetText(GStrings.GetString("PICKER_TEAM"));

	AddressPortHintLabel->SetText(GStrings.GetString("PICKER_PORTHINT"));
	TeamHintLabel->SetText(GStrings.GetString("PICKER_TEAMHINT"));
#ifdef BIASEDDOOM_ONLINE
	OnlineCheckbox->SetText("Join Online Relay by code");
	OnlineConnectivityHint->SetText("Relay is automatic; a verified direct route is used when available. No port forwarding.");
	#else
	OnlineCheckbox->SetText("Online Relay is unavailable in this build");
	OnlineConnectivityHint->SetText("Build with online support to join through the relay service.");
	#endif
	OnlineServiceLabel->SetText("Online service URL");
	OnlineCodeLabel->SetText("Join code");
	OnlineRecentLabel->SetText("Recent");
	OnlinePasswordLabel->SetText("Session password (not saved)");
	OnlineReviewButton->SetText("Review host mods");
	if (!HasCurrentOnlineReview())
		OnlineReviewLabel->SetText("Review the host's mod plan before download approval is available.");
	OnlineAcceptModsCheckbox->SetText("I approve downloading and mounting the reviewed host mods");
}

void JoinSubPage::OnGeometryChanged()
{
	const double w = GetWidth();
	const double h = GetHeight();

	constexpr double LabelOfsSize = 100.0;
	constexpr double hintOfs = 160.0;

	double y = 0.0;

	AddressLabel->SetFrameGeometry(0.0, y, LabelOfsSize, AddressLabel->GetPreferredHeight());
	AddressEdit->SetFrameGeometry(LabelOfsSize, y, w * 0.5 - LabelOfsSize, EditHeight);
	y += EditHeight + 2.0;

	AddressPortLabel->SetFrameGeometry(0.0, y, LabelOfsSize, AddressPortLabel->GetPreferredHeight());
	AddressPortEdit->SetFrameGeometry(LabelOfsSize, y, 55.0, EditHeight);

	AddressPortHintLabel->SetFrameGeometry(hintOfs, y, w - hintOfs, AddressPortHintLabel->GetPreferredHeight());
	y += EditHeight + 7.0;

	OnlineCheckbox->SetFrameGeometry(0.0, y, w, OnlineCheckbox->GetPreferredHeight());
	y += OnlineCheckbox->GetPreferredHeight() + 2.0;
	OnlineConnectivityHint->SetFrameGeometry(0.0, y, w, OnlineConnectivityHint->GetPreferredHeight());
	y += OnlineConnectivityHint->GetPreferredHeight() + 2.0;
	OnlineServiceLabel->SetFrameGeometry(0.0, y, LabelOfsSize, OnlineServiceLabel->GetPreferredHeight());
	OnlineServiceEdit->SetFrameGeometry(LabelOfsSize, y, w - LabelOfsSize, EditHeight);
	y += EditHeight + 2.0;
	OnlineCodeLabel->SetFrameGeometry(0.0, y, LabelOfsSize, OnlineCodeLabel->GetPreferredHeight());
	OnlineCodeEdit->SetFrameGeometry(LabelOfsSize, y, w * 0.5 - LabelOfsSize, EditHeight);
	y += EditHeight + 2.0;
	OnlineRecentLabel->SetFrameGeometry(0.0, y, LabelOfsSize, OnlineRecentLabel->GetPreferredHeight());
	OnlineRecentDropdown->SetFrameGeometry(LabelOfsSize, y, w - LabelOfsSize, OnlineRecentDropdown->GetPreferredHeight());
	y += OnlineRecentDropdown->GetPreferredHeight() + 2.0;
	OnlinePasswordLabel->SetFrameGeometry(0.0, y, w * 0.5, OnlinePasswordLabel->GetPreferredHeight());
	OnlinePasswordEdit->SetFrameGeometry(w * 0.5, y, w * 0.5, EditHeight);
	y += EditHeight + 2.0;
	OnlineReviewButton->SetFrameGeometry(0.0, y, 160.0, OnlineReviewButton->GetPreferredHeight());
	y += OnlineReviewButton->GetPreferredHeight() + 2.0;
	OnlineReviewLabel->SetFrameGeometry(0.0, y, w, OnlineReviewLabel->GetPreferredHeight());
	y += OnlineReviewLabel->GetPreferredHeight() + 2.0;
	if (OnlineReviewList->IsVisible())
	{
		OnlineReviewList->SetFrameGeometry(0.0, y, w, 80.0);
		y += 84.0;
	}
	OnlineAcceptModsCheckbox->SetFrameGeometry(0.0, y, w, OnlineAcceptModsCheckbox->GetPreferredHeight());
	y += OnlineAcceptModsCheckbox->GetPreferredHeight() + 7.0;

	TeamDeathmatchLabel->SetFrameGeometry(0.0, y, w, TeamDeathmatchLabel->GetPreferredHeight());
	y += TeamDeathmatchLabel->GetPreferredHeight();

	TeamLabel->SetFrameGeometry(0.0, y, LabelOfsSize, TeamLabel->GetPreferredHeight());
	TeamEdit->SetFrameGeometry(LabelOfsSize, y, 55.0, EditHeight);
	TeamHintLabel->SetFrameGeometry(hintOfs, y, w - hintOfs, TeamHintLabel->GetPreferredHeight());

	MainTab->UpdatePlayButton();
}

BrowseSubPage::BrowseSubPage(NetworkPage* main, const FStartupSelectionInfo& info) : Widget(nullptr), MainTab(main)
{
	ServiceLabel = new TextLabel(this);
	ServiceEdit = new LineEdit(this);
	ServiceEdit->SetText(info.DefaultOnlineService.GetChars());
	ServiceEdit->SetMaxLength(255);
	ModeFilterLabel = new TextLabel(this);
	ModeFilterEdit = new LineEdit(this);
	ModeFilterEdit->SetMaxLength(32);
	RegionFilterLabel = new TextLabel(this);
	RegionFilterEdit = new LineEdit(this);
	RegionFilterEdit->SetMaxLength(32);
	MinPlayersFilterLabel = new TextLabel(this);
	MinPlayersFilterEdit = new LineEdit(this);
	MinPlayersFilterEdit->SetNumericMode();
	MinPlayersFilterEdit->SetMaxLength(2);
	PasswordFilterLabel = new TextLabel(this);
	PasswordFilterDropdown = new Dropdown(this);
	PasswordFilterDropdown->AddItem("Any access");
	PasswordFilterDropdown->AddItem("Open only");
	PasswordFilterDropdown->AddItem("Password only");
	ContentFilterLabel = new TextLabel(this);
	ContentFilterDropdown = new Dropdown(this);
	ContentFilterDropdown->AddItem("Any content");
	ContentFilterDropdown->AddItem("Base content only");
	ContentFilterDropdown->AddItem("Modded only");
	ReportReasonDropdown = new Dropdown(this);
	ReportReasonDropdown->AddItem("Report reason...");
	ReportReasonDropdown->AddItem("Spam or misleading listing");
	ReportReasonDropdown->AddItem("Abusive session title");
	ReportReasonDropdown->AddItem("Unsafe or illegal content");
	ReportReasonDropdown->SetSelectedItem(0);
	const std::string storedBlocks = static_cast<const char*>(defaultonlineblocked);
	for (size_t start = 0; start < storedBlocks.size() && BlockedCodes.Size() < 64; )
	{
		const size_t end = storedBlocks.find(',', start);
		const auto code = storedBlocks.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (IsSafeOnlineJoinCode(code) && !IsBlocked(code.c_str()))
			BlockedCodes.Push(code.c_str());
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	RefreshButton = new PushButton(this);
	RefreshButton->OnClick = [this]() { Refresh(); };
	SessionsList = new ListView(this);
	SessionsList->SetColumnWidths({ 0.27, 0.15, 0.14, 0.12, 0.16, 0.16 });
	SessionsList->OnActivated = [this]() { JoinSelected(); };
	StatusLabel = new TextLabel(this);
	JoinButton = new PushButton(this);
	JoinButton->OnClick = [this]() { JoinSelected(); };
	BlockButton = new PushButton(this);
	BlockButton->OnClick = [this]() { BlockSelected(); };
	ReportButton = new PushButton(this);
	ReportButton->OnClick = [this]() { ReportSelected(); };
	ClearBlocksButton = new PushButton(this);
	ClearBlocksButton->OnClick = [this]() { ClearBlockedSessions(); };
}

void BrowseSubPage::UpdateLanguage()
{
	ServiceLabel->SetText("Online service URL");
	ModeFilterLabel->SetText("Mode");
	RegionFilterLabel->SetText("Region");
	MinPlayersFilterLabel->SetText("Players ≥");
	PasswordFilterLabel->SetText("Access");
	ContentFilterLabel->SetText("Content");
	RefreshButton->SetText("Refresh public sessions");
	JoinButton->SetText("Use selected session");
	BlockButton->SetText("Block selected session");
	ReportButton->SetText("Report selected session");
	ClearBlocksButton->SetText("Clear local blocked sessions");
	if (Listings.Size() == 0)
		StatusLabel->SetText("Enter the official service URL, then refresh.");
}

void BrowseSubPage::Refresh()
{
	while (SessionsList->GetItemAmount() != 0)
		SessionsList->RemoveItem(0);
	Listings.Clear();
#ifdef BIASEDDOOM_ONLINE
	BdOnline::FOnlineServiceConfig config;
	config.BaseUrl = ServiceEdit->GetText();
	config.ConnectTimeoutSeconds = 3;
	config.RequestTimeoutSeconds = 8;
	BdOnline::FOnlineServiceClient client(std::move(config));
	std::vector<BdOnline::FOnlineSessionListing> sessions;
	std::string error;
	if (!client.ListSessions(sessions, error))
	{
		StatusLabel->SetText(("Could not refresh public sessions: " + error).c_str());
		return;
	}
	const std::string modeFilter = ModeFilterEdit->GetText();
	const std::string regionFilter = RegionFilterEdit->GetText();
	const int minimumPlayers = clamp<int>(MinPlayersFilterEdit->GetTextInt(), 0, MAXPLAYERS);
	const int passwordFilter = PasswordFilterDropdown->GetSelectedItem();
	const int contentFilter = ContentFilterDropdown->GetSelectedItem();
	int shown = 0;
	for (const auto& session : sessions)
	{
		if (IsBlocked(session.JoinCode.c_str()))
			continue;
		if (!MatchesOnlineBrowseFilter(session.GameMode, modeFilter) || !MatchesOnlineBrowseFilter(session.Region, regionFilter) ||
			session.Players < static_cast<uint32_t>(minimumPlayers) ||
			(passwordFilter == 1 && session.PasswordRequired) || (passwordFilter == 2 && !session.PasswordRequired) ||
			(contentFilter == 1 && session.Modded) || (contentFilter == 2 && !session.Modded))
			continue;
		SessionsList->AddItem(session.Title);
		const int row = static_cast<int>(SessionsList->GetItemAmount() - 1);
		SessionsList->UpdateItem(session.GameMode, row, 1);
		SessionsList->UpdateItem(session.Region, row, 2);
		std::string playerCount = std::to_string(session.Players) + "/" + std::to_string(session.MaxPlayers);
		if (session.ReservedPlayers != 0)
			playerCount += " (" + std::to_string(session.ReservedPlayers) + " reconnecting)";
		SessionsList->UpdateItem(playerCount, row, 3);
		SessionsList->UpdateItem(session.Modded ? "Modded" : "Base content", row, 4);
		std::string access = session.PasswordRequired ? "Password" : "Open";
		if (session.LobbyOpen)
			access += " · Lobby";
		else
			access += session.AllowJoinInProgress ? " · In progress (joinable)" : " · In progress (closed)";
		SessionsList->UpdateItem(access, row, 5);
		Listings.Push({ session.SessionId.c_str(), session.JoinCode.c_str(), session.LobbyOpen, session.AllowJoinInProgress });
		++shown;
	}
	StatusLabel->SetText(shown == 0 ? "No public sessions match these filters." : "Columns: Title · Mode · Region · Players · Content · Access. Select a session to continue in the Join tab.");
#else
	StatusLabel->SetText("This build was made without online multiplayer support.");
#endif
}

void BrowseSubPage::JoinSelected()
{
	const int selected = SessionsList->GetSelectedItem();
	if (selected < 0 || selected >= static_cast<int>(Listings.Size()))
	{
		StatusLabel->SetText("Select a public session first.");
		return;
	}
	if (!Listings[selected].LobbyOpen && !Listings[selected].AllowJoinInProgress)
	{
		StatusLabel->SetText("This game is already in progress and does not accept new players.");
		return;
	}
	if (!Listings[selected].LobbyOpen)
		StatusLabel->SetText("Joining game in progress: content will be verified, then the current world will synchronize before you enter.");
	MainTab->SelectOnlineSession(ServiceEdit->GetText(), Listings[selected].JoinCode);
}

bool BrowseSubPage::IsBlocked(const FString& joinCode) const
{
	for (const auto& blocked : BlockedCodes)
	{
		if (!blocked.CompareNoCase(joinCode))
			return true;
	}
	return false;
}

void BrowseSubPage::SaveBlockedCodes() const
{
	std::string encoded;
	for (const auto& blocked : BlockedCodes)
	{
		if (!encoded.empty())
			encoded += ',';
		encoded += blocked.GetChars();
	}
	defaultonlineblocked = encoded.c_str();
}

void BrowseSubPage::BlockSelected()
{
	const int selected = SessionsList->GetSelectedItem();
	if (selected < 0 || selected >= static_cast<int>(Listings.Size()))
	{
		StatusLabel->SetText("Select a public session first.");
		return;
	}
	const FString& joinCode = Listings[selected].JoinCode;
	if (!IsSafeOnlineJoinCode(joinCode.GetChars()))
	{
		StatusLabel->SetText("This listing has an invalid join code and cannot be blocked.");
		return;
	}
	if (!IsBlocked(joinCode))
	{
		if (BlockedCodes.Size() >= 64)
		{
			StatusLabel->SetText("Your local block list is full (64 sessions).");
			return;
		}
		BlockedCodes.Push(joinCode);
		SaveBlockedCodes();
	}
	Refresh();
}

void BrowseSubPage::ReportSelected()
{
	const int selected = SessionsList->GetSelectedItem();
	if (selected < 0 || selected >= static_cast<int>(Listings.Size()))
	{
		StatusLabel->SetText("Select a public session first.");
		return;
	}
	static constexpr const char* Reasons[] = {
		nullptr,
		"spam or misleading public listing",
		"abusive public session title",
		"unsafe or illegal public content",
	};
	constexpr int ReasonCount = 4;
	const int reasonIndex = ReportReasonDropdown->GetSelectedItem();
	if (reasonIndex <= 0 || reasonIndex >= ReasonCount)
	{
		StatusLabel->SetText("Choose a report reason first.");
		return;
	}
	std::string error;
	if (!I_OnlineReportSession(ServiceEdit->GetText(), Listings[selected].SessionId.GetChars(), Reasons[reasonIndex], error))
	{
		StatusLabel->SetText(("Could not submit report: " + error).c_str());
		return;
	}
	bool hiddenLocally = IsBlocked(Listings[selected].JoinCode);
	if (!hiddenLocally && BlockedCodes.Size() < 64)
	{
		BlockedCodes.Push(Listings[selected].JoinCode);
		SaveBlockedCodes();
		hiddenLocally = true;
	}
	Refresh();
	StatusLabel->SetText(hiddenLocally ? "Report received. This session is now hidden on this device." :
		"Report received. Your local block list is full, so this session remains visible.");
}

void BrowseSubPage::ClearBlockedSessions()
{
	if (BlockedCodes.Size() == 0)
	{
		StatusLabel->SetText("No public sessions are currently blocked on this device.");
		return;
	}
	BlockedCodes.Clear();
	SaveBlockedCodes();
	Refresh();
}

void BrowseSubPage::OnGeometryChanged()
{
	const double w = GetWidth();
	const double h = GetHeight();
	double y = 0.0;
	ServiceLabel->SetFrameGeometry(0.0, y, 120.0, ServiceLabel->GetPreferredHeight());
	ServiceEdit->SetFrameGeometry(120.0, y, w - 120.0, EditHeight);
	y += EditHeight + 4.0;
	ModeFilterLabel->SetFrameGeometry(0.0, y, 40.0, ModeFilterLabel->GetPreferredHeight());
	ModeFilterEdit->SetFrameGeometry(40.0, y, 110.0, EditHeight);
	RegionFilterLabel->SetFrameGeometry(158.0, y, 48.0, RegionFilterLabel->GetPreferredHeight());
	RegionFilterEdit->SetFrameGeometry(206.0, y, 110.0, EditHeight);
	MinPlayersFilterLabel->SetFrameGeometry(324.0, y, 88.0, MinPlayersFilterLabel->GetPreferredHeight());
	MinPlayersFilterEdit->SetFrameGeometry(412.0, y, 54.0, EditHeight);
	y += EditHeight + 2.0;
	PasswordFilterLabel->SetFrameGeometry(0.0, y, 48.0, PasswordFilterLabel->GetPreferredHeight());
	PasswordFilterDropdown->SetFrameGeometry(48.0, y, 145.0, PasswordFilterDropdown->GetPreferredHeight());
	ContentFilterLabel->SetFrameGeometry(201.0, y, 54.0, ContentFilterLabel->GetPreferredHeight());
	ContentFilterDropdown->SetFrameGeometry(255.0, y, 155.0, ContentFilterDropdown->GetPreferredHeight());
	y += std::max(PasswordFilterDropdown->GetPreferredHeight(), ContentFilterDropdown->GetPreferredHeight()) + 4.0;
	RefreshButton->SetFrameGeometry(0.0, y, 180.0, RefreshButton->GetPreferredHeight());
	JoinButton->SetFrameGeometry(188.0, y, 180.0, JoinButton->GetPreferredHeight());
	BlockButton->SetFrameGeometry(376.0, y, 180.0, BlockButton->GetPreferredHeight());
	y += std::max(RefreshButton->GetPreferredHeight(), std::max(JoinButton->GetPreferredHeight(), BlockButton->GetPreferredHeight())) + 4.0;
	ReportReasonDropdown->SetFrameGeometry(0.0, y, 210.0, ReportReasonDropdown->GetPreferredHeight());
	ReportButton->SetFrameGeometry(218.0, y, 180.0, ReportButton->GetPreferredHeight());
	y += std::max(ReportReasonDropdown->GetPreferredHeight(), ReportButton->GetPreferredHeight()) + 4.0;
	ClearBlocksButton->SetFrameGeometry(0.0, y, 220.0, ClearBlocksButton->GetPreferredHeight());
	y += ClearBlocksButton->GetPreferredHeight() + 4.0;
	StatusLabel->SetFrameGeometry(0.0, y, w, StatusLabel->GetPreferredHeight());
	y += StatusLabel->GetPreferredHeight() + 4.0;
	SessionsList->SetFrameGeometry(0.0, y, w, std::max(60.0, h - y));
}
