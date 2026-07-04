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
    TSharedPtr<SEditableTextBox> ServerUrlBox;
    TSharedPtr<SEditableTextBox> RepositoryBox;
    TSharedPtr<SEditableTextBox> IdentityBox;
    TSharedPtr<STextBlock>       StatusText;

    FReply OnConnectClicked();
    FReply OnDisconnectClicked();
    void OnConnectSuccess();

    void HandleRepoMissingOnServer(FLoreSourceControlProvider* Provider);
    void HandleCloneConflict(FLoreSourceControlProvider* Provider, const FString& RepoUrl);
    void PushLocalFilesToRepo(FLoreSourceControlProvider* Provider);

    bool LocalRepoExists(FLoreSourceControlProvider* Provider) const;
    bool LocalFolderHasProjectFiles() const;
    bool IsServerRepoEmpty(FLoreSourceControlProvider* Provider, const FString& RepoUrl) const;

    FLoreSourceControlProvider* GetProvider() const;
};