#pragma once

#include "CoreMinimal.h"
#include "ISourceControlState.h"
#include "Styling/SlateBrush.h"

class FLoreSourceControlState : public ISourceControlState
{
public:
    explicit FLoreSourceControlState(const FString& InFilePath)
        : FilePath(InFilePath)
        , bIsModified(false)
        , bIsAdded(false)
        , bIsDeleted(false)
        , bIsLocked(false)
        , bIsUnknown(true)
    {}

    virtual const FString& GetFilename() const override
        { return FilePath; }

    virtual bool IsCheckedOut() const override
        { return bIsModified || bIsLocked; }

    virtual bool IsCheckedOutOther(FString* Who = nullptr) const override
        { return false; }

    virtual bool IsCheckedOutInOtherBranch(
        const FString& CurrentBranch = TEXT("")) const override
        { return false; }

    virtual bool IsModifiedInOtherBranch(
        const FString& CurrentBranch = TEXT("")) const override
        { return false; }

    virtual bool IsCheckedOutOrModifiedInOtherBranch(
        const FString& CurrentBranch = TEXT("")) const override
        { return false; }

    virtual TArray<FString> GetCheckedOutBranches() const override
        { return {}; }

    virtual FString GetOtherUserBranchCheckedOuts() const override
        { return TEXT(""); }

    virtual bool GetOtherBranchHeadModification(
        FString& HeadBranchOut,
        FString& ActionOut,
        int32& HeadChangeListOut) const override
        { return false; }

    virtual bool IsCurrent() const override      { return true; }
    virtual bool IsSourceControlled() const override { return !bIsUnknown; }
    virtual bool IsAdded() const override        { return bIsAdded; }
    virtual bool IsDeleted() const override      { return bIsDeleted; }
    virtual bool IsIgnored() const override      { return false; }
    virtual bool IsModified() const override     { return bIsModified; }
    virtual bool IsUnknown() const override      { return bIsUnknown; }
    virtual bool IsConflicted() const override   { return false; }

    virtual bool CanCheckIn() const override
        { return bIsModified || bIsAdded || bIsDeleted; }
    virtual bool CanCheckout() const override
        { return !bIsModified && !bIsUnknown; }
    virtual bool CanEdit() const override        { return true; }
    virtual bool CanDelete() const override      { return IsSourceControlled(); }
    virtual bool CanAdd() const override         { return bIsUnknown; }
    virtual bool CanRevert() const override      { return bIsModified; }

    virtual FText GetDisplayName() const override;
    virtual FText GetDisplayTooltip() const override;

    // UE 5.8 uses FSlateIcon
    virtual FSlateIcon GetIcon() const override;

    virtual const FDateTime& GetTimeStamp() const override
        { return TimeStamp; }

    // History
    virtual int32 GetHistorySize() const override { return 0; }
    virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
        GetHistoryItem(int32 HistoryIndex) const override { return nullptr; }
    virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
        FindHistoryRevision(int32 RevisionNumber) const override { return nullptr; }
    virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
        FindHistoryRevision(const FString& InRevision) const override { return nullptr; }
    virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
        GetCurrentRevision() const override { return nullptr; }

    // -------------------------------------------------------
    // Lore-specific data
    // -------------------------------------------------------
    FString   FilePath;
    bool      bIsModified;
    bool      bIsAdded;
    bool      bIsDeleted;
    bool      bIsLocked;
    bool      bIsUnknown;
    FDateTime TimeStamp;
};