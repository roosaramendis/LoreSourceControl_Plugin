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

#define LOCTEXT_NAMESPACE "SLoreSettings"

void SLoreSettings::OnConnectFucked(const FString& Output,
    const FString& RepoUrl, FLoreSourceControlProvider* Provider)
{
    StatusText->SetText(FText::Format(
            LOCTEXT("Failed", "Failed: {0}"),
            FText::FromString(Output)
        ));
        StatusText->SetColorAndOpacity(
            FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)));
            
        /*FString RepoUrl = Provider->ServerUrl
                    + TEXT("/")
                    + Provider->RepositoryName;*/

        FText DialogTitle   = LOCTEXT("CreateRepoTitle",
            "Repository Not Found");
        FText DialogMessage = FText::Format(
            LOCTEXT("CreateRepoMsg",
                "Repository '{0}' does not exist on the server.\n\nWould you like to create it?"),
            FText::FromString(RepoUrl)
        );

        EAppReturnType::Type createRopo_orNah = FMessageDialog::Open(
            EAppMsgType::YesNo,
            DialogMessage,
            DialogTitle
        );    // red
        if (createRopo_orNah == EAppReturnType::Yes)
        {
            // Step 3 — create the repo
            StatusText->SetText(LOCTEXT("Creating", "Creating repository..."));

            FString CreateOutput = Provider->RunLoreCommand({
                TEXT("repository"), TEXT("create"), RepoUrl
            });

            bool bCreated = !CreateOutput.Contains(TEXT("Error"))
                        && !CreateOutput.Contains(TEXT("error"));

            if (bCreated)
            {
                OnConnectSuccess();
            }
            else
            {
                StatusText->SetText(FText::Format(
                    LOCTEXT("CreateFailed", "Failed to create: {0}"),
                    FText::FromString(CreateOutput)
                ));
                StatusText->SetColorAndOpacity(
                    FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)));
            }
        }
        else
        {
            // User said No
            StatusText->SetText(LOCTEXT("Cancelled", "Cancelled"));
            StatusText->SetColorAndOpacity(
                FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)));
        }
}

void SLoreSettings::OnConnectSuccess()
{
    FLoreSourceControlProvider* Provider = GetProvider();
    if (!Provider) return;

    // Save to config so it persists between sessions
    GConfig->SetString(
        TEXT("LoreSourceControl"),
        TEXT("ServerUrl"),
        *Provider->ServerUrl,
        GEditorSettingsIni
    );
    GConfig->SetString(
        TEXT("LoreSourceControl"),
        TEXT("RepositoryName"),
        *Provider->RepositoryName,
        GEditorSettingsIni
    );
    GConfig->SetString(
        TEXT("LoreSourceControl"),
        TEXT("Identity"),
        *Provider->Identity,
        GEditorSettingsIni
    );
    GConfig->Flush(false, GEditorSettingsIni);

    Provider->SetConnected(true);

    StatusText->SetText(FText::Format(
        LOCTEXT("Connected", "Connected to {0}/{1}"),
        FText::FromString(Provider->ServerUrl),
        FText::FromString(Provider->RepositoryName)
    ));
    StatusText->SetColorAndOpacity(
        FSlateColor(FLinearColor(0.2f, 0.8f, 0.2f)));
}

void SLoreSettings::Construct(const FArguments& InArgs)
{
    // SNew(SVerticalBox) stacks everything top to bottom.
    // Each + SVerticalBox::Slot() is one row.

    FString SavedServer, SavedRepo, SavedIdentity;
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("ServerUrl"), SavedServer, GEditorSettingsIni);
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("RepositoryName"), SavedRepo, GEditorSettingsIni);
    GConfig->GetString(TEXT("LoreSourceControl"),
        TEXT("Identity"), SavedIdentity, GEditorSettingsIni);



    ChildSlot
    [
        SNew(SVerticalBox)

        // ---- Title row ----------------------------------------
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 0.f, 0.f, 8.f)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("Title", "Lore Source Control"))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
        ]

        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 0.f, 0.f, 8.f)
        [
            SNew(SSeparator)
        ]

        // ---- Server URL row -----------------------------------
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 4.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .VAlign(VAlign_Center)
            .FillWidth(0.3f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ServerLabel", "Server URL"))
            ]

            + SHorizontalBox::Slot()
            .FillWidth(0.7f)
            [
                SAssignNew(ServerUrlBox, SEditableTextBox)
                .HintText(LOCTEXT("ServerHint", "lore://127.0.0.1:41337"))
                .Text(FText::FromString(SavedServer.IsEmpty() 
                    ? TEXT("lore://127.0.0.1:41337") : SavedServer))
            ]
        ]

        // ---- Repository row -----------------------------------
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 4.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .VAlign(VAlign_Center)
            .FillWidth(0.3f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("RepoLabel", "Repository"))
            ]

            + SHorizontalBox::Slot()
            .FillWidth(0.7f)
            [
                SAssignNew(RepositoryBox, SEditableTextBox)
                .HintText(LOCTEXT("RepoHint", "MyProject"))
                .Text(FText::FromString(SavedRepo))
            ]
        ]

        // ---- Identity row ------------------------------------
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 4.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .VAlign(VAlign_Center)
            .FillWidth(0.3f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("IdentityLabel", "Identity"))
            ]

            + SHorizontalBox::Slot()
            .FillWidth(0.7f)
            [
                SAssignNew(IdentityBox, SEditableTextBox)
                .HintText(LOCTEXT("IdentityHint", "you@studio.com"))
                .Text(FText::FromString(SavedIdentity))
            ]
        ]

        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 8.f)
        [
            SNew(SSeparator)
        ]

        // ---- Status text ------------------------------------
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 4.f)
        [
            SAssignNew(StatusText, STextBlock)
            .Text(LOCTEXT("StatusIdle", "Not connected"))
            .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
        ]

        // ---- Buttons row ------------------------------------
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 8.f, 0.f, 0.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.f, 0.f, 8.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("ConnectBtn", "Connect"))
                .OnClicked(this, &SLoreSettings::OnConnectClicked)
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("DisconnectBtn", "Disconnect"))
                .OnClicked(this, &SLoreSettings::OnDisconnectClicked)
            ]
        ]
    ];
}

FReply SLoreSettings::OnConnectClicked()
{
    FLoreSourceControlProvider* Provider = GetProvider();
    if (!Provider) return FReply::Handled();

    // Read values from the text boxes
    Provider->ServerUrl      = ServerUrlBox->GetText().ToString();
    Provider->RepositoryName = RepositoryBox->GetText().ToString();
    Provider->Identity       = IdentityBox->GetText().ToString();

    // Show "connecting..." while we run the CLI
    StatusText->SetText(LOCTEXT("Connecting", "Connecting..."));
    StatusText->SetColorAndOpacity(
        FSlateColor(FLinearColor(1.f, 1.f, 0.f))); // yellow

    // Build the lore repository create command:
    // lore repository create lore://127.0.0.1:41337/MyProject
    FString RepoUrl = Provider->ServerUrl + TEXT("/") + Provider->RepositoryName;
    FString Output = Provider->RunLoreCommand({
        TEXT("status")
    });

    // Check if it worked
    bool bSuccess = Output.Contains(TEXT("Repository"))
                 && !Output.Contains(TEXT("ERROR")) 
                 && !Output.Contains(TEXT("error"))
                 && !Output.IsEmpty();

    if (bSuccess)
    {
        OnConnectSuccess();
    }
    else
    {
        OnConnectFucked(Output, RepoUrl, Provider);
    }

    return FReply::Handled();
}

FReply SLoreSettings::OnDisconnectClicked()
{
    FLoreSourceControlProvider* Provider = GetProvider();
    if (Provider) Provider->Close();

    StatusText->SetText(LOCTEXT("Disconnected", "Disconnected"));
    StatusText->SetColorAndOpacity(
        FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f))); // grey

    return FReply::Handled();
}

FLoreSourceControlProvider* SLoreSettings::GetProvider() const
{
    FLoreSourceControlModule* Module = FModuleManager::GetModulePtr
        <FLoreSourceControlModule>("LoreSourceControl");
    return Module ? &Module->GetProvider() : nullptr;
}

#undef LOCTEXT_NAMESPACE