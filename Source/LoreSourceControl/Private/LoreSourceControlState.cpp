#include "LoreSourceControlState.h"

#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Textures/SlateIcon.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl.State"

void FLoreSourceControlState::ApplyStatus(const FLoreFileStatus& Status)
{
	WorkingState = Status.State;
	bStaged = Status.bStaged;
	bIsCurrent = Status.bIsCurrent;
	if (Status.bLockKnown)
	{
		LockState = Status.Lock;
		LockOwner = Status.LockOwner;
	}
	if (WorkingState == ELoreWorkingState::Outside || WorkingState == ELoreWorkingState::NotTracked)
	{
		LockState = ELoreLockState::None; // can't be locked
		LockOwner.Reset();
	}
	TimeStamp = FDateTime::Now();
}

// "Checked out" in Perforce terms: I hold the lock, or I already have local
// changes (Lore lets you edit without a lock, and those files must still
// show up in Submit Content).
bool FLoreSourceControlState::IsCheckedOut() const
{
	return LockState == ELoreLockState::Mine
		|| WorkingState == ELoreWorkingState::Added
		|| WorkingState == ELoreWorkingState::Deleted
		|| WorkingState == ELoreWorkingState::Modified;
}

bool FLoreSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (LockState != ELoreLockState::Other)
	{
		return false;
	}
	if (Who)
	{
		*Who = LockOwner.IsEmpty() ? FString(TEXT("another user")) : LockOwner;
	}
	return true;
}

bool FLoreSourceControlState::IsSourceControlled() const
{
	return WorkingState == ELoreWorkingState::Unchanged
		|| WorkingState == ELoreWorkingState::Modified
		|| WorkingState == ELoreWorkingState::Added
		|| WorkingState == ELoreWorkingState::Deleted;
}

bool FLoreSourceControlState::IsModified() const
{
	return WorkingState == ELoreWorkingState::Modified
		|| WorkingState == ELoreWorkingState::Added
		|| WorkingState == ELoreWorkingState::Deleted;
}

bool FLoreSourceControlState::CanCheckIn() const
{
	return IsModified() && LockState != ELoreLockState::Other;
}

bool FLoreSourceControlState::CanCheckout() const
{
	return (WorkingState == ELoreWorkingState::Unchanged || WorkingState == ELoreWorkingState::Modified)
		&& LockState == ELoreLockState::None
		&& bIsCurrent;
}

bool FLoreSourceControlState::CanEdit() const
{
	return IsCheckedOut() || !IsSourceControlled();
}

bool FLoreSourceControlState::CanDelete() const
{
	return IsSourceControlled()
		&& LockState != ELoreLockState::Other
		&& WorkingState != ELoreWorkingState::Deleted;
}

bool FLoreSourceControlState::CanRevert() const
{
	return IsModified() || LockState == ELoreLockState::Mine;
}

FText FLoreSourceControlState::GetDisplayName() const
{
	if (LockState == ELoreLockState::Other)
	{
		return FText::Format(LOCTEXT("CheckedOutOther", "Checked out by {0}"),
			FText::FromString(LockOwner.IsEmpty() ? FString(TEXT("another user")) : LockOwner));
	}
	switch (WorkingState)
	{
	case ELoreWorkingState::Added:      return LOCTEXT("Added", "Marked for add");
	case ELoreWorkingState::Deleted:    return LOCTEXT("Deleted", "Marked for delete");
	case ELoreWorkingState::NotTracked: return LOCTEXT("NotTracked", "Not in Lore");
	case ELoreWorkingState::Outside:    return LOCTEXT("Outside", "Outside the Lore repository");
	case ELoreWorkingState::Unknown:    return LOCTEXT("Unknown", "Unknown");
	default: break;
	}
	if (!bIsCurrent)
	{
		return LOCTEXT("NotCurrent", "Newer version on server");
	}
	if (LockState == ELoreLockState::Mine)
	{
		return LOCTEXT("CheckedOut", "Checked out");
	}
	if (WorkingState == ELoreWorkingState::Modified)
	{
		return LOCTEXT("ModifiedNoLock", "Modified (not checked out)");
	}
	return LOCTEXT("Unchanged", "Up to date");
}

FText FLoreSourceControlState::GetDisplayTooltip() const
{
	if (LockState == ELoreLockState::Other)
	{
		return FText::Format(LOCTEXT("CheckedOutOtherTip", "Checked out by {0}. You can't check it out or submit it until they submit or revert."),
			FText::FromString(LockOwner.IsEmpty() ? FString(TEXT("another user")) : LockOwner));
	}
	switch (WorkingState)
	{
	case ELoreWorkingState::Added:      return LOCTEXT("AddedTip", "New file, will be added on the next submit.");
	case ELoreWorkingState::Deleted:    return LOCTEXT("DeletedTip", "Deleted locally, the delete will be submitted on the next submit.");
	case ELoreWorkingState::NotTracked: return LOCTEXT("NotTrackedTip", "This file is not in Lore yet. Use Mark For Add, or just submit it.");
	case ELoreWorkingState::Outside:    return LOCTEXT("OutsideTip", "This file is not inside the project's Lore repository.");
	case ELoreWorkingState::Unknown:    return LOCTEXT("UnknownTip", "Status not known yet.");
	default: break;
	}
	if (!bIsCurrent)
	{
		return LOCTEXT("NotCurrentTip", "Someone submitted a newer version. Sync before checking it out.");
	}
	if (LockState == ELoreLockState::Mine)
	{
		return WorkingState == ELoreWorkingState::Modified
			? LOCTEXT("CheckedOutModifiedTip", "Checked out by you, with local changes.")
			: LOCTEXT("CheckedOutTip", "Checked out by you.");
	}
	if (WorkingState == ELoreWorkingState::Modified)
	{
		return LOCTEXT("ModifiedNoLockTip", "Changed locally but not checked out. Someone else could check it out; check it out before continuing.");
	}
	return LOCTEXT("UnchangedTip", "Up to date with Lore.");
}

FSlateIcon FLoreSourceControlState::GetIcon() const
{
	const FName Style = FRevisionControlStyleManager::GetStyleSetName();

	if (LockState == ELoreLockState::Other)            { return FSlateIcon(Style, "RevisionControl.CheckedOutByOtherUser"); }
	if (WorkingState == ELoreWorkingState::Added)      { return FSlateIcon(Style, "RevisionControl.OpenForAdd"); }
	if (WorkingState == ELoreWorkingState::Deleted)    { return FSlateIcon(Style, "RevisionControl.MarkedForDelete"); }
	if (WorkingState == ELoreWorkingState::NotTracked) { return FSlateIcon(Style, "RevisionControl.NotInDepot"); }
	if (!bIsCurrent)                                   { return FSlateIcon(Style, "RevisionControl.NotAtHeadRevision"); }
	if (LockState == ELoreLockState::Mine)             { return FSlateIcon(Style, "RevisionControl.CheckedOut"); }
	if (WorkingState == ELoreWorkingState::Modified)   { return FSlateIcon(Style, "RevisionControl.ModifiedLocally"); }
	return FSlateIcon();
}

#undef LOCTEXT_NAMESPACE
