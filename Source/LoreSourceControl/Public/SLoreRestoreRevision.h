#pragma once

// "Restore Revision..." dialog: lists the revisions of one asset, and puts the
// chosen one back on disk as a local change (check out + old content).

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SWindow;

struct FLoreRevisionRow
{
	int32 Number = 0;
	FString User;
	FString Message;
	FDateTime Date;
	bool bIsCurrent = false;
};

class SLoreRestoreRevision : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLoreRestoreRevision) {}
		SLATE_ARGUMENT(FString, AssetLabel)
		SLATE_ARGUMENT(TArray<TSharedPtr<FLoreRevisionRow>>, Rows)
		SLATE_ARGUMENT(TWeakPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Entry point from the Content Browser menu. PackageName like "/Game/maps/map_1".
	static void OpenFor(const FString& PackageName);

	// Revision number the user picked, 0 if cancelled.
	int32 GetChosenRevision() const { return ChosenRevision; }

private:
	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FLoreRevisionRow> Item, const TSharedRef<STableViewBase>& OwnerTable);
	FReply OnRestoreClicked();
	FReply OnCancelClicked();
	bool CanRestore() const;

	TArray<TSharedPtr<FLoreRevisionRow>> Rows;
	TSharedPtr<SListView<TSharedPtr<FLoreRevisionRow>>> ListView;
	TWeakPtr<SWindow> ParentWindow;
	int32 ChosenRevision = 0;
};
