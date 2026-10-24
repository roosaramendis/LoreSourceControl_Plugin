#include "SLoreRestoreRevision.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlRevision.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SLoreRestoreRevision"

void SLoreRestoreRevision::Construct(const FArguments& InArgs)
{
	Rows = InArgs._Rows;
	ParentWindow = InArgs._ParentWindow;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("Header", "Pick the revision of {0} to bring back.\nIt is restored as a local change: Submit to make it the newest revision, or Revert to undo."),
				FText::FromString(InArgs._AssetLabel)))
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8.0f, 0.0f)
		[
			SAssignNew(ListView, SListView<TSharedPtr<FLoreRevisionRow>>)
			.ListItemsSource(&Rows)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SLoreRestoreRevision::OnGenerateRow)
		]

		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Restore", "Restore Selected Revision"))
				.IsEnabled(this, &SLoreRestoreRevision::CanRestore)
				.OnClicked(this, &SLoreRestoreRevision::OnRestoreClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "Cancel"))
				.OnClicked(this, &SLoreRestoreRevision::OnCancelClicked)
			]
		]
	];
}

TSharedRef<ITableRow> SLoreRestoreRevision::OnGenerateRow(TSharedPtr<FLoreRevisionRow> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FText Label = FText::Format(LOCTEXT("RowFormat", "Revision {0}{1}    {2}    {3}    {4}"),
		FText::AsNumber(Item->Number, &FNumberFormattingOptions::DefaultNoGrouping()),
		Item->bIsCurrent ? LOCTEXT("CurrentTag", " (current)") : FText::GetEmpty(),
		FText::AsDateTime(Item->Date, EDateTimeStyle::Short, EDateTimeStyle::Short),
		FText::FromString(Item->User),
		FText::FromString(Item->Message.Replace(TEXT("\n"), TEXT("  "))));

	return SNew(STableRow<TSharedPtr<FLoreRevisionRow>>, OwnerTable)
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(STextBlock).Text(Label)
		];
}

bool SLoreRestoreRevision::CanRestore() const
{
	if (!ListView.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FLoreRevisionRow>> Selected = ListView->GetSelectedItems();
	return Selected.Num() == 1 && !Selected[0]->bIsCurrent;
}

FReply SLoreRestoreRevision::OnRestoreClicked()
{
	const TArray<TSharedPtr<FLoreRevisionRow>> Selected = ListView->GetSelectedItems();
	if (Selected.Num() == 1)
	{
		ChosenRevision = Selected[0]->Number;
	}
	if (TSharedPtr<SWindow> Window = ParentWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	return FReply::Handled();
}

FReply SLoreRestoreRevision::OnCancelClicked()
{
	ChosenRevision = 0;
	if (TSharedPtr<SWindow> Window = ParentWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	return FReply::Handled();
}

void SLoreRestoreRevision::OpenFor(const FString& PackageName)
{
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	const TArray<FString> Filenames = SourceControlHelpers::PackageFilenames({ PackageName });
	if (Filenames.Num() != 1)
	{
		return;
	}
	const FString Filename = FPaths::ConvertRelativePathToFull(Filenames[0]);

	// 1. Get the history (same request the History window makes).
	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateStatus = ISourceControlOperation::Create<FUpdateStatus>();
	UpdateStatus->SetUpdateHistory(true);
	(void)Provider.Execute(UpdateStatus, TArray<FString>{ Filename });

	FSourceControlStatePtr State = Provider.GetState(Filename, EStateCacheUsage::Use);
	TArray<TSharedPtr<FLoreRevisionRow>> RevisionRows;
	if (State.IsValid())
	{
		for (int32 Index = 0; Index < State->GetHistorySize(); ++Index)
		{
			TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Revision = State->GetHistoryItem(Index);
			if (!Revision.IsValid())
			{
				continue;
			}
			TSharedPtr<FLoreRevisionRow> Row = MakeShared<FLoreRevisionRow>();
			Row->Number = Revision->GetRevisionNumber();
			Row->User = Revision->GetUserName();
			Row->Message = Revision->GetDescription();
			Row->Date = Revision->GetDate();
			Row->bIsCurrent = Index == 0;
			RevisionRows.Add(Row);
		}
	}
	if (RevisionRows.Num() < 2)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoHistory", "This asset has no older revisions to restore."), LOCTEXT("NoHistoryTitle", "Restore Revision"));
		return;
	}

	// 2. Let the user pick one (modal window).
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(LOCTEXT("WindowTitle", "Restore Revision - {0}"), FText::FromString(FPaths::GetBaseFilename(Filename))))
		.SizingRule(ESizingRule::UserSized)
		.ClientSize(FVector2D(800.0f, 400.0f))
		.SupportsMinimize(false);

	TSharedRef<SLoreRestoreRevision> Picker = SNew(SLoreRestoreRevision)
		.AssetLabel(PackageName)
		.Rows(RevisionRows)
		.ParentWindow(Window);
	Window->SetContent(Picker);
	FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());

	const int32 Chosen = Picker->GetChosenRevision();
	if (Chosen <= 0)
	{
		return;
	}

	// 3. Unload the asset, write the old revision over it, reload it.
	const FString RevisionText = FString::FromInt(Chosen);
	bool bRestored = false;
	(void)USourceControlHelpers::ApplyOperationAndReloadPackages({ Filename },
		[&Provider, &RevisionText, &bRestored](const TArray<FString>& InFiles) -> bool
		{
			TSharedRef<FSync, ESPMode::ThreadSafe> Sync = ISourceControlOperation::Create<FSync>();
			Sync->SetRevision(RevisionText);
			bRestored = Provider.Execute(Sync, InFiles) == ECommandResult::Succeeded;
			return bRestored;
		},
		/*bReloadWorld=*/ true);

	if (bRestored)
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::Format(LOCTEXT("Restored", "Revision {0} restored and checked out.\nSubmit it to make it the newest revision, or Revert to go back."), FText::AsNumber(Chosen)),
			LOCTEXT("RestoredTitle", "Restore Revision"));
	}
}

#undef LOCTEXT_NAMESPACE
