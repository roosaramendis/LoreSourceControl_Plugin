#pragma once

// The object Unreal talks to for every revision control action.
//
// Game thread only. Real work (running `lore`) happens in FLoreCommand on a
// dedicated single worker thread, so the editor never freezes on a slow
// server, and lore commands never run at the same time as each other.

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "ISourceControlOperation.h"
#include "LoreUtils.h"

class FLoreSourceControlState;
class FLoreCommand;
class FQueuedThreadPool;
struct FAssetData;

struct FLoreSettings
{
	FString LoreBinary;      // empty = auto-detect
	FString ServerUrl;       // lore://host:port
	FString RepositoryName;  // used by "Set Up Repository"
	FString Identity;        // e.g. you@studio.com
};

class FLoreSourceControlProvider : public ISourceControlProvider
{
public:
	FLoreSourceControlProvider();
	virtual ~FLoreSourceControlProvider() override;

	// ---- ISourceControlProvider ----
	virtual const FName& GetName() const override;
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual FText GetStatusText() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual TMap<EStatus, FString> GetStatus() const override;

	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override { return false; }
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRoot) override {}
	virtual int32 GetStateBranchIndex(const FString& /*InBranchName*/) const override { return INDEX_NONE; }
	virtual bool GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const override { return false; }

	virtual ECommandResult::Type GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;

	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;

	virtual ECommandResult::Type Execute(
		const FSourceControlOperationRef& InOperation,
		FSourceControlChangelistPtr InChangelist,
		const TArray<FString>& InFiles,
		EConcurrency::Type InConcurrency = EConcurrency::Synchronous,
		const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;

	virtual bool CanExecuteOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual bool CanCancelOperation(const FSourceControlOperationRef& InOperation) const override { return false; }
	virtual void CancelOperation(const FSourceControlOperationRef& InOperation) override {}

	virtual TArray<TSharedRef<class ISourceControlLabel>> GetLabels(const FString& InMatchingSpec) const override;
	virtual TArray<FSourceControlChangelistRef> GetChangelists(EStateCacheUsage::Type InStateCacheUsage) override;

	virtual bool UsesLocalReadOnlyState() const override      { return false; }
	virtual bool UsesChangelists() const override             { return false; }
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

	// ---- used by the settings panel ----
	const FLoreSettings& GetSettings() const { return Settings; }
	void SetSettings(const FLoreSettings& InSettings, bool bSave);
	FLoreContext MakeContext() const;
	void RefreshRepoRoot();
	bool IsRepoFound() const { return bRepoFound; }
	const FString& GetRepoRoot() const { return RepoRoot; }
	void StartConnectCheck();
	bool IsBusy() const { return bConnectInFlight || CommandQueue.Num() > 0; }

private:
	void LoadSettings();
	void SaveSettings() const;
	FQueuedThreadPool* GetWorkerPool();
	void ProcessFinishedQueue();

	void HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath);

	TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> GetStateInternal(const FString& AbsPath);
	void ApplyResults(FLoreCommand& Command);
	ECommandResult::Type ProcessCompletedCommand(FLoreCommand& Command);
	void ReportMessages(const FLoreCommand& Command) const;

	FName ProviderName;
	TSharedRef<FLoreLockStore, ESPMode::ThreadSafe> LockStore;
	TSharedRef<FLoreMoveStore, ESPMode::ThreadSafe> MoveStore; // renames waiting to be submitted (new -> old)
	FDelegateHandle AssetRenamedHandle;
	FLoreSettings Settings;

	// Repository on disk
	FString RepoRoot;          // folder containing .lore (or the project folder if none)
	bool bRepoFound = false;
	FString ConfigRemoteUrl;   // from .lore/config.toml
	FString ConfigIdentity;    // from .lore/config.toml

	// Connection / server info
	bool bAvailable = false;
	bool bConnectInFlight = false;
	bool bRefreshInFlight = false;
	bool bHasConnectedOnce = false;
	FString RepoId;
	FString BranchName;
	TOptional<int32> LocalRevision;
	TOptional<int32> RemoteRevision;
	double LastRefreshTime = 0.0;

	TMap<FString, TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>> StateCache;

	// Async commands waiting for Tick() to pick up their results.
	TArray<FLoreCommand*> CommandQueue;
	FQueuedThreadPool* WorkerPool = nullptr;

	FSourceControlStateChanged OnSourceControlStateChanged;
};
