#pragma once

// What the editor shows for one file (icon, tooltip, which menu entries are
// enabled). Only touched on the game thread.

#include "CoreMinimal.h"
#include "ISourceControlState.h"
#include "LoreUtils.h"

class FLoreSourceControlState : public ISourceControlState
{
public:
	explicit FLoreSourceControlState(const FString& InFilename)
		: Filename(InFilename)
	{
	}

	// Copies a worker result into this state.
	void ApplyStatus(const FLoreFileStatus& Status);

	// ---- ISourceControlState ----
	virtual const FString& GetFilename() const override { return Filename; }
	virtual const FDateTime& GetTimeStamp() const override { return TimeStamp; }

	virtual bool IsCheckedOut() const override;
	virtual bool IsCheckedOutOther(FString* Who = nullptr) const override;
	virtual bool IsCheckedOutInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual bool IsModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual TArray<FString> GetCheckedOutBranches() const override { return {}; }
	virtual FString GetOtherUserBranchCheckedOuts() const override { return FString(); }
	virtual bool GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const override { return false; }

	virtual bool IsCurrent() const override { return bIsCurrent; }
	virtual bool IsSourceControlled() const override;
	virtual bool IsAdded() const override { return WorkingState == ELoreWorkingState::Added; }
	virtual bool IsDeleted() const override { return WorkingState == ELoreWorkingState::Deleted; }
	virtual bool IsIgnored() const override { return false; }
	virtual bool IsModified() const override;
	virtual bool IsUnknown() const override { return WorkingState == ELoreWorkingState::Unknown; }
	virtual bool IsConflicted() const override { return false; }

	virtual bool CanCheckIn() const override;
	virtual bool CanCheckout() const override;
	virtual bool CanEdit() const override;
	virtual bool CanDelete() const override;
	virtual bool CanAdd() const override { return WorkingState == ELoreWorkingState::NotTracked; }
	virtual bool CanRevert() const override;

	virtual FText GetDisplayName() const override;
	virtual FText GetDisplayTooltip() const override;
	virtual FSlateIcon GetIcon() const override;

	// History is Phase 2.
	virtual int32 GetHistorySize() const override { return 0; }
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32 HistoryIndex) const override { return nullptr; }
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32 RevisionNumber) const override { return nullptr; }
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString& InRevision) const override { return nullptr; }
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetCurrentRevision() const override { return nullptr; }

	// ---- Lore data ----
	FString Filename;
	ELoreWorkingState WorkingState = ELoreWorkingState::Unknown;
	bool bStaged = false;
	ELoreLockState LockState = ELoreLockState::None;
	FString LockOwner;
	bool bIsCurrent = true;
	FDateTime TimeStamp;
};
