#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "ISourceControlOperation.h"

class FLoreSourceControlState;

class FLoreSourceControlProvider : public ISourceControlProvider
{
public:
    FLoreSourceControlProvider();
    
    virtual const FName& GetName() const override;
    virtual void Init(bool bForceConnection = true) override;
    virtual void Close() override;
    virtual FText GetStatusText() const override;
    virtual bool IsEnabled() const override;
    virtual bool IsAvailable() const override;
    virtual TMap<EStatus, FString> GetStatus() const override;

    virtual bool QueryStateBranchConfig(
        const FString& ConfigSrc, const FString& ConfigDest) override;
    virtual void RegisterStateBranches(
        const TArray<FString>& BranchNames,
        const FString& ContentRoot) override;
    virtual int32 GetStateBranchIndex(
        const FString& BranchName) const override;
    virtual bool GetStateBranchAtIndex(
        int32 BranchIndex, FString& OutBranchName) const override;

    virtual ECommandResult::Type GetState(
        const TArray<FString>& InFiles,
        TArray<FSourceControlStateRef>& OutState,
        EStateCacheUsage::Type InStateCacheUsage) override;

    virtual ECommandResult::Type GetState(
        const TArray<FSourceControlChangelistRef>& InChangelists,
        TArray<FSourceControlChangelistStateRef>& OutState,
        EStateCacheUsage::Type InStateCacheUsage) override;

    virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(
        TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;

    virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(
        const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
    virtual void UnregisterSourceControlStateChanged_Handle(
        FDelegateHandle Handle) override;

    virtual ECommandResult::Type Execute(
        const FSourceControlOperationRef& InOperation,
        FSourceControlChangelistPtr InChangelist,
        const TArray<FString>& InFiles,
        EConcurrency::Type InConcurrency = EConcurrency::Synchronous,
        const FSourceControlOperationComplete& InOperationCompleteDelegate
            = FSourceControlOperationComplete()) override;

    virtual bool CanExecuteOperation(
        const FSourceControlOperationRef& InOperation) const override;
    virtual bool CanCancelOperation(
        const FSourceControlOperationRef& InOperation) const override;
    virtual void CancelOperation(
        const FSourceControlOperationRef& InOperation) override;

    virtual TArray<TSharedRef<class ISourceControlLabel>> GetLabels(
        const FString& InMatchingSpec) const override;
    virtual TArray<FSourceControlChangelistRef> GetChangelists(
        EStateCacheUsage::Type InStateCacheUsage) override;

    virtual bool UsesLocalReadOnlyState() const override      { return false; }
    virtual bool UsesChangelists() const override             { return true;  }
    virtual bool UsesUncontrolledChangelists() const override { return false; }
    virtual bool UsesCheckout() const override                { return true;  }
    virtual bool UsesFileRevisions() const override           { return true;  }
    virtual bool UsesSnapshots() const override               { return false; }
    virtual bool UsesSoftRevertOnDelete() const override      { return false; }
    virtual bool AllowsDiffAgainstDepot() const override      { return true;  }

    virtual TOptional<bool> HasChangesToSync() const override;
    virtual TOptional<bool> HasChangesToCheckIn() const override;
    virtual void Tick() override;

    virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;

    FString ServerUrl;
    FString RepositoryName;
    FString Identity;

    FString RunLoreCommand(const TArray<FString>& Args) const;
    void SetConnected(bool bConnected) { bIsConnected = bConnected; }
    void ShowReconnectNotification();
private:
    FName ProviderName;
    bool bIsConnected = false;
    bool bPendingStartupCheck = false;
    FString StatusMessage;
    FSourceControlStateChanged OnSourceControlStateChanged;

    // Cache of file path -> state
    TMap<FString, TSharedRef<FLoreSourceControlState>> StateCache;

    // Parse lore status output and update the cache
    void UpdateStateCache(const TArray<FString>& InFiles);
};

