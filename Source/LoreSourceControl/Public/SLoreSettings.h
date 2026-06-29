#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Misc/MessageDialog.h"

class FLoreSourceControlProvider;

class SLoreSettings : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLoreSettings) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    // -------------------------------------------------------
    // Widget references — we keep these so we can
    // read their values when Connect is clicked
    // -------------------------------------------------------
    TSharedPtr<SEditableTextBox> ServerUrlBox;
    TSharedPtr<SEditableTextBox> RepositoryBox;
    TSharedPtr<SEditableTextBox> IdentityBox;
    TSharedPtr<STextBlock>       StatusText;

    // -------------------------------------------------------
    // Button callbacks
    // -------------------------------------------------------
    FReply OnConnectClicked();
    FReply OnDisconnectClicked();
    void OnConnectSuccess();
    void OnConnectFucked(const FString& Output, const FString& RepoUrl,
                        FLoreSourceControlProvider* Provider);

    // -------------------------------------------------------
    // Helper — get our provider
    // -------------------------------------------------------
    FLoreSourceControlProvider* GetProvider() const;
};