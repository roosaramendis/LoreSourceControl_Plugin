#include "SLoreSettings.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlModule.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Misc/MessageDialog.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"

#define LOCTEXT_NAMESPACE "SLoreSettings"

void SLoreSettings::OnConnectSuccess()
{
    FLoreSourceControlProvider* Provider = GetProvider();
    if (!Provider) return;

    GConfig->SetString(TEXT("LoreSourceControl"), TEXT("ServerUrl"),
        *Provider->ServerUrl, GEditorSettingsIni);
    GConfig->SetString(TEXT("LoreSourceControl"), TEXT("RepositoryName"),
        *Provider->RepositoryName, GEditorSettingsIni);
    GConfig->SetString(TEXT("LoreSourceControl"), TEXT("Identity"),
        *Provider->Identity, GEditorSettingsIni);
    GConfig->Flush(false, GEditorSettingsIni);

    Provider->SetConnected(true);

    StatusText->SetText(FText::Format(
        LOCTEXT("Connected", "Connected to {0}/{1}"),
        FText::FromString(Provider->ServerUrl),
        FText::FromString(Provider->RepositoryName)
    ));
    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.2f, 0.8f, 0.2f)));
}

void SLoreSettings::Construct(const FArguments& InArgs)
{
    FString SavedServer, SavedRepo, SavedIdentity;
    GConfig->GetString(TEXT("LoreSourceControl"), TEXT("ServerUrl"), SavedServer, GEditorSettingsIni);
    GConfig->GetString(TEXT("LoreSourceControl"), TEXT("RepositoryName"), SavedRepo, GEditorSettingsIni);
    GConfig->GetString(TEXT("LoreSourceControl"), TEXT("Identity"), SavedIdentity, GEditorSettingsIni);

    ChildSlot
    [
        SNew(SVerticalBox)

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("Title", "Lore Source Control"))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
        ]

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
        [ SNew(SSeparator) ]

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().VAlign(VAlign_Center).FillWidth(0.3f)
            [ SNew(STextBlock).Text(LOCTEXT("ServerLabel", "Server URL")) ]
            + SHorizontalBox::Slot().FillWidth(0.7f)
            [
                SAssignNew(ServerUrlBox, SEditableTextBox)
                .HintText(LOCTEXT("ServerHint", "lore://127.0.0.1:41337"))
                .Text(FText::FromString(SavedServer.IsEmpty()
                    ? TEXT("lore://127.0.0.1:41337") : SavedServer))
            ]
        ]

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().VAlign(VAlign_Center).FillWidth(0.3f)
            [ SNew(STextBlock).Text(LOCTEXT("RepoLabel", "Repository")) ]
            + SHorizontalBox::Slot().FillWidth(0.7f)
            [
                SAssignNew(RepositoryBox, SEditableTextBox)
                .HintText(LOCTEXT("RepoHint", "MyProject"))
                .Text(FText::FromString(SavedRepo))
            ]
        ]

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().VAlign(VAlign_Center).FillWidth(0.3f)
            [ SNew(STextBlock).Text(LOCTEXT("IdentityLabel", "Identity")) ]
            + SHorizontalBox::Slot().FillWidth(0.7f)
            [
                SAssignNew(IdentityBox, SEditableTextBox)
                .HintText(LOCTEXT("IdentityHint", "you@studio.com"))
                .Text(FText::FromString(SavedIdentity))
            ]
        ]

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f)
        [ SNew(SSeparator) ]

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
        [
            SAssignNew(StatusText, STextBlock)
            .Text(LOCTEXT("StatusIdle", "Not connected"))
            .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
        ]

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("ConnectBtn", "Connect"))
                .OnClicked(this, &SLoreSettings::OnConnectClicked)
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("DisconnectBtn", "Disconnect"))
                .OnClicked(this, &SLoreSettings::OnDisconnectClicked)
            ]
        ]
    ];
}

bool SLoreSettings::LocalFolderHasProjectFiles() const
{
    TArray<FString> FoundFiles;
    IFileManager::Get().FindFilesRecursive(
        FoundFiles, *FPaths::ProjectContentDir(), TEXT("*.*"), true, false);
    return FoundFiles.Num() > 0;
}

bool SLoreSettings::LocalRepoExists(FLoreSourceControlProvider* Provider) const
{
    FString StatusOutput = Provider->RunLoreCommand({ TEXT("status") });
    return StatusOutput.Contains(TEXT("Repository"))
        && !StatusOutput.Contains(TEXT("Error"));
}

bool SLoreSettings::IsServerRepoEmpty(FLoreSourceControlProvider* Provider,
    const FString& RepoUrl) const
{
    FString HistoryOutput = Provider->RunLoreCommand({
        TEXT("history"), RepoUrl, TEXT("--identity"), Provider->Identity
    });
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: History check RAW: %s"), *HistoryOutput);

    bool bLooksEmpty = HistoryOutput.IsEmpty()
        || HistoryOutput.Contains(TEXT("No revisions"))
        || HistoryOutput.Contains(TEXT("0 revisions"));

    return bLooksEmpty;
}

void SLoreSettings::PushLocalFilesToRepo(FLoreSourceControlProvider* Provider)
{
    StatusText->SetText(LOCTEXT("Staging", "Staging existing project files..."));
    FString StageOutput = Provider->RunLoreCommand({ TEXT("stage"), TEXT("--scan"), TEXT(".") });
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Stage: %s"), *StageOutput);

    StatusText->SetText(LOCTEXT("Committing", "Creating initial commit..."));
    FString CommitOutput = Provider->RunLoreCommand({
        TEXT("commit"), TEXT("Initial commit"), TEXT("--identity"), Provider->Identity
    });
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Commit: %s"), *CommitOutput);

    StatusText->SetText(LOCTEXT("Pushing", "Pushing to server..."));
    FString PushOutput = Provider->RunLoreCommand({ TEXT("push"), TEXT("--identity"), Provider->Identity });
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Push: %s"), *PushOutput);

    OnConnectSuccess();
}

FReply SLoreSettings::OnConnectClicked()
{
    FLoreSourceControlProvider* Provider = GetProvider();
    if (!Provider) return FReply::Handled();

    Provider->ServerUrl      = ServerUrlBox->GetText().ToString();
    Provider->RepositoryName = RepositoryBox->GetText().ToString();
    Provider->Identity       = IdentityBox->GetText().ToString();

    StatusText->SetText(LOCTEXT("Connecting", "Connecting..."));
    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 0.f)));

    // Step 1 — is the server reachable? MUST pass --identity or Lore
    // treats us as anonymous and returns an empty repo list every time.
    FString ServerCheck = Provider->RunLoreCommand({
        TEXT("repository"), TEXT("list"), Provider->ServerUrl,
        TEXT("--identity"), Provider->Identity
    });
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Server check: %s"), *ServerCheck);

    bool bServerReachable = !ServerCheck.Contains(TEXT("Error"))
                         && !ServerCheck.Contains(TEXT("transport error"))
                         && !ServerCheck.Contains(TEXT("connect"));

    if (!bServerReachable)
    {
        StatusText->SetText(FText::Format(
            LOCTEXT("ServerUnreachable", "Cannot reach server: {0}"),
            FText::FromString(Provider->ServerUrl)));
        StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)));
        return FReply::Handled();
    }

    FString RepoUrl = Provider->ServerUrl + TEXT("/") + Provider->RepositoryName;
    bool bRepoOnServer = ServerCheck.Contains(Provider->RepositoryName);

    // Step 2 — repo doesn't exist on server at all
    if (!bRepoOnServer)
    {
        HandleRepoMissingOnServer(Provider);
        return FReply::Handled();
    }

    // Step 3 — repo exists on server. Does local already have it?
    if (LocalRepoExists(Provider))
    {
        OnConnectSuccess();
        return FReply::Handled();
    }

    // Step 4 — no local repo yet. If the server repo is empty (fresh
    // repo, never pushed to), it's safe to push local files into it
    // regardless of what's in the local folder.
    if (IsServerRepoEmpty(Provider, RepoUrl))
    {
        PushLocalFilesToRepo(Provider);
        return FReply::Handled();
    }

    // Step 5 — server repo has real content. Local folder has unrelated files?
    if (LocalFolderHasProjectFiles())
    {
        HandleCloneConflict(Provider, RepoUrl);
        return FReply::Handled();
    }

    // Empty folder, server has real content — safe to clone
    StatusText->SetText(LOCTEXT("Cloning", "Cloning from server..."));
    FString CloneOutput = Provider->RunLoreCommand({
        TEXT("clone"), RepoUrl, TEXT("."), TEXT("--identity"), Provider->Identity
    });
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Clone: %s"), *CloneOutput);

    bool bCloneOk = !CloneOutput.Contains(TEXT("Error"))
                 && !CloneOutput.Contains(TEXT("error"));

    if (!bCloneOk)
    {
        StatusText->SetText(FText::Format(
            LOCTEXT("CloneFailed", "Failed to clone: {0}"),
            FText::FromString(CloneOutput)));
        StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)));
        return FReply::Handled();
    }

    OnConnectSuccess();
    return FReply::Handled();
}

void SLoreSettings::HandleRepoMissingOnServer(FLoreSourceControlProvider* Provider)
{
    FText DialogMessage = FText::Format(
        LOCTEXT("RepoMissingMsg",
            "Repository '{0}' does not exist on the server. Would you like to create it? This is safe, it will only add Lore tracking to this folder and will not modify or delete any of your existing files."),
        FText::FromString(Provider->RepositoryName));

    EAppReturnType::Type Choice = FMessageDialog::Open(
        EAppMsgType::YesNo, DialogMessage,
        LOCTEXT("RepoMissingTitle", "Repository Not Found"));

    if (Choice != EAppReturnType::Yes)
    {
        StatusText->SetText(LOCTEXT("Cancelled", "Cancelled"));
        StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)));
        return;
    }

    FString RepoUrl = Provider->ServerUrl + TEXT("/") + Provider->RepositoryName;

    StatusText->SetText(LOCTEXT("Creating", "Creating repository..."));
    FString CreateOutput = Provider->RunLoreCommand({
        TEXT("repository"), TEXT("create"), RepoUrl,
        TEXT("--identity"), Provider->Identity
    });
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Create: %s"), *CreateOutput);

    bool bCreated = !CreateOutput.Contains(TEXT("Error"))
                 && !CreateOutput.Contains(TEXT("error"));

    if (!bCreated)
    {
        StatusText->SetText(FText::Format(
            LOCTEXT("CreateFailed", "Failed to create: {0}"),
            FText::FromString(CreateOutput)));
        StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)));
        return;
    }

    PushLocalFilesToRepo(Provider);
}

void SLoreSettings::HandleCloneConflict(FLoreSourceControlProvider* Provider,
    const FString& RepoUrl)
{
    FText DialogMessage = FText::Format(
        LOCTEXT("CloneConflictMsg",
            "The server already has a repository named '{0}' with existing content, and this folder already has project files not tracked by that repository. Choose Yes to delete the server repository and create a new one from THIS folder's files instead (use only if the server repo is unwanted). Choose No to cancel; nothing will be touched, and you can clone the server repo into a different empty folder instead."),
        FText::FromString(Provider->RepositoryName));

    EAppReturnType::Type Choice = FMessageDialog::Open(
        EAppMsgType::YesNo, DialogMessage,
        LOCTEXT("CloneConflictTitle", "Repository Conflict"));

    if (Choice != EAppReturnType::Yes)
    {
        StatusText->SetText(LOCTEXT("Cancelled",
            "Cancelled. Clone into a new empty folder to merge manually"));
        StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)));
        return;
    }

    EAppReturnType::Type Confirm = FMessageDialog::Open(
        EAppMsgType::YesNo,
        LOCTEXT("ConfirmDeleteMsg",
            "This will permanently delete the existing repository and its full history on the server. This cannot be undone. Are you absolutely sure?"),
        LOCTEXT("ConfirmDeleteTitle", "Confirm Server Repository Deletion"));

    if (Confirm != EAppReturnType::Yes)
    {
        StatusText->SetText(LOCTEXT("Cancelled", "Cancelled"));
        StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)));
        return;
    }

    StatusText->SetText(LOCTEXT("Deleting", "Deleting old server repository..."));
    FString DeleteOutput = Provider->RunLoreCommand({
        TEXT("repository"), TEXT("delete"), RepoUrl,
        TEXT("--identity"), Provider->Identity
    });
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Delete: %s"), *DeleteOutput);

    bool bDeleted = !DeleteOutput.Contains(TEXT("Error"))
                 && !DeleteOutput.Contains(TEXT("error"));

    if (!bDeleted)
    {
        StatusText->SetText(FText::Format(
            LOCTEXT("DeleteFailed", "Failed to delete old repository: {0}"),
            FText::FromString(DeleteOutput)));
        StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)));
        return;
    }

    HandleRepoMissingOnServer(Provider);
}

FReply SLoreSettings::OnDisconnectClicked()
{
    FLoreSourceControlProvider* Provider = GetProvider();
    if (Provider) Provider->Close();

    StatusText->SetText(LOCTEXT("Disconnected", "Disconnected"));
    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)));

    return FReply::Handled();
}

FLoreSourceControlProvider* SLoreSettings::GetProvider() const
{
    FLoreSourceControlModule* Module = FModuleManager::GetModulePtr
        <FLoreSourceControlModule>("LoreSourceControl");
    return Module ? &Module->GetProvider() : nullptr;
}

#undef LOCTEXT_NAMESPACE