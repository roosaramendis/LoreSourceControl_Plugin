#pragma once

// The panel shown in "Revision Control > Change Revision Control Provider"
// when Lore is selected.

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "LoreUtils.h"
#include "LoreSourceControlProvider.h" // FLoreSettings

// What the background check found (filled on a worker thread).
struct FLoreProbeResult
{
	bool bLocalRepo = false;
	FString LocalRepoId;
	FString LocalError;
	bool bServerOk = false;
	FString ServerError;
	TArray<TPair<FString, FString>> ServerRepos; // name -> id
};

class SLoreSettings : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLoreSettings) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> MakeField(const FText& Label, const FText& Hint, const FText& Tooltip, FString FLoreSettings::* Field);

	FText GetRepoInfoText() const;
	bool IsIdle() const { return !bBusy; }

	FReply OnCheckConnection();
	FReply OnSetUpRepository();

	void StartProbe(bool bForSetup);
	void OnProbeDone(const FLoreProbeResult& Result, const FLoreContext& Ctx, bool bForSetup);
	void StartCreate(const FLoreContext& Ctx);
	void OnCreateDone(bool bOk, const FString& Message);

	void SetStatus(const FString& Message, const FLinearColor& Color);

	bool bBusy = false;
	FString StatusMessage;
	FLinearColor StatusColor = FLinearColor::White;
};
