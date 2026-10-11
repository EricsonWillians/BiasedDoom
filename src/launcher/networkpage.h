#pragma once

#include <zwidget/core/widget.h>
#include "gstrings.h"
#include "tarray.h"

#include <cstdint>

class LauncherWindow;
class CheckboxLabel;
class LineEdit;
class Dropdown;
class TextLabel;
class PushButton;
class TabWidget;
class ListView;
struct WadStuff;
struct FStartupSelectionInfo;

class HostSubPage;
class JoinSubPage;
class BrowseSubPage;

class NetworkPage : public Widget
{
public:
	NetworkPage(LauncherWindow* launcher, const FStartupSelectionInfo& info);
	void UpdateLanguage();
	void UpdatePlayButton();
	bool IsInHost() const;
	void SetValues(FStartupSelectionInfo& info) const;
	void InitializeTabs(const FStartupSelectionInfo& info);
	void SelectOnlineSession(const FString& serviceUrl, const FString& joinCode);

private:
	void OnGeometryChanged() override;
	void OnSetFocus() override;
	void OnIWADsListActivated();

	LauncherWindow* Launcher = nullptr;

	Dropdown* IWADsDropdown = nullptr;
	TextLabel* SaveFileLabel = nullptr;
	LineEdit* SaveFileEdit = nullptr;
	CheckboxLabel* SaveFileCheckbox = nullptr;
	
	HostSubPage* HostPage = nullptr;
	JoinSubPage* JoinPage = nullptr;
	BrowseSubPage* BrowsePage = nullptr;
	TabWidget* StartPages = nullptr;
	
	TextLabel* ParametersLabel = nullptr;
	LineEdit* ParametersEdit = nullptr;
	CheckboxLabel* SaveParametersCheckbox = nullptr;
};

class HostSubPage : public Widget
{
public:
	HostSubPage(NetworkPage* main, const FStartupSelectionInfo& info);
	void UpdateLanguage();
	void SetValues(FStartupSelectionInfo& info) const;

private:
	void OnGeometryChanged() override;

	NetworkPage* MainTab = nullptr;

	TextLabel* NetModesLabel = nullptr;
	Dropdown* NetModesDropdown = nullptr;
	TextLabel* TicDupLabel = nullptr;
	Dropdown* TicDupDropdown = nullptr;
	CheckboxLabel* ExtraTicCheckbox = nullptr;

	TextLabel* GameModesLabel = nullptr;
	Dropdown* GameModesDropdown = nullptr;
	CheckboxLabel* AltDeathmatchCheckbox = nullptr;
	TextLabel* TeamLabel = nullptr;
	LineEdit* TeamEdit = nullptr;
	TextLabel* TeamHintLabel = nullptr;

	TextLabel* MaxPlayersLabel = nullptr;
	LineEdit* MaxPlayersEdit = nullptr;
	TextLabel* MaxPlayerHintLabel = nullptr;
	TextLabel* PortLabel = nullptr;
	LineEdit* PortEdit = nullptr;
	TextLabel* PortHintLabel = nullptr;

	CheckboxLabel* OnlineCheckbox = nullptr;
	TextLabel* OnlineConnectivityHint = nullptr;
	TextLabel* OnlineServiceLabel = nullptr;
	LineEdit* OnlineServiceEdit = nullptr;
	TextLabel* OnlineTitleLabel = nullptr;
	LineEdit* OnlineTitleEdit = nullptr;
	TextLabel* OnlineRegionLabel = nullptr;
	LineEdit* OnlineRegionEdit = nullptr;
	TextLabel* OnlinePasswordLabel = nullptr;
	LineEdit* OnlinePasswordEdit = nullptr;
	CheckboxLabel* OnlinePublicCheckbox = nullptr;
	TextLabel* OnlineModSummary = nullptr;
};

class JoinSubPage : public Widget
{
public:
	JoinSubPage(NetworkPage* main, const FStartupSelectionInfo& info);
	void UpdateLanguage();
	void SetValues(FStartupSelectionInfo& info);
	void SetOnlineSession(const FString& serviceUrl, const FString& joinCode);

private:
	void OnGeometryChanged() override;

	NetworkPage* MainTab = nullptr;
	struct FRecentSession
	{
		FString ServiceUrl;
		FString JoinCode;
	};
	void OnRecentSessionChanged(int selection);
	void RememberOnlineSession();
	void SaveRecentSessions() const;
	void ReviewOnlineContent();
	void ResetOnlineReview();
	bool HasCurrentOnlineReview() const;

	TextLabel* AddressLabel = nullptr;
	LineEdit* AddressEdit = nullptr;
	TextLabel* AddressPortLabel = nullptr;
	LineEdit* AddressPortEdit = nullptr;
	TextLabel* AddressPortHintLabel = nullptr;

	CheckboxLabel* OnlineCheckbox = nullptr;
	TextLabel* OnlineConnectivityHint = nullptr;
	TextLabel* OnlineServiceLabel = nullptr;
	LineEdit* OnlineServiceEdit = nullptr;
	TextLabel* OnlineCodeLabel = nullptr;
	LineEdit* OnlineCodeEdit = nullptr;
	TextLabel* OnlinePasswordLabel = nullptr;
	LineEdit* OnlinePasswordEdit = nullptr;
	PushButton* OnlineReviewButton = nullptr;
	TextLabel* OnlineReviewLabel = nullptr;
	ListView* OnlineReviewList = nullptr;
	CheckboxLabel* OnlineAcceptModsCheckbox = nullptr;
	TextLabel* OnlineRecentLabel = nullptr;
	Dropdown* OnlineRecentDropdown = nullptr;
	
	TextLabel* TeamDeathmatchLabel = nullptr;
	TextLabel* TeamLabel = nullptr;
	LineEdit* TeamEdit = nullptr;
	TextLabel* TeamHintLabel = nullptr;
	TArray<FRecentSession> RecentSessions;
	uint64_t OnlineReviewRevision = 0;
	uint64_t ReviewedOnlineRevision = 0;
	bool OnlineReviewComplete = false;
	FString ReviewedContentRootDigest;
	FString ReviewedHostPeerId;
};

class BrowseSubPage : public Widget
{
public:
	BrowseSubPage(NetworkPage* main, const FStartupSelectionInfo& info);
	void UpdateLanguage();

private:
	void Refresh();
	void JoinSelected();
	void BlockSelected();
	void ReportSelected();
	void ClearBlockedSessions();
	bool IsBlocked(const FString& joinCode) const;
	void SaveBlockedCodes() const;
	void OnGeometryChanged() override;

	struct FListing
	{
		FString SessionId;
		FString JoinCode;
		bool LobbyOpen = false;
		bool AllowJoinInProgress = false;
	};

	NetworkPage* MainTab = nullptr;
	TextLabel* ServiceLabel = nullptr;
	LineEdit* ServiceEdit = nullptr;
	TextLabel* ModeFilterLabel = nullptr;
	LineEdit* ModeFilterEdit = nullptr;
	TextLabel* RegionFilterLabel = nullptr;
	LineEdit* RegionFilterEdit = nullptr;
	TextLabel* MinPlayersFilterLabel = nullptr;
	LineEdit* MinPlayersFilterEdit = nullptr;
	TextLabel* PasswordFilterLabel = nullptr;
	Dropdown* PasswordFilterDropdown = nullptr;
	TextLabel* ContentFilterLabel = nullptr;
	Dropdown* ContentFilterDropdown = nullptr;
	Dropdown* ReportReasonDropdown = nullptr;
	PushButton* RefreshButton = nullptr;
	ListView* SessionsList = nullptr;
	TextLabel* StatusLabel = nullptr;
	PushButton* JoinButton = nullptr;
	PushButton* BlockButton = nullptr;
	PushButton* ReportButton = nullptr;
	PushButton* ClearBlocksButton = nullptr;
	TArray<FListing> Listings;
	TArray<FString> BlockedCodes;
};
