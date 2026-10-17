#pragma once

// One unit of work for the background worker thread.
//
// Flow:
//   game thread : provider creates FLoreCommand, copies everything it needs
//   worker      : DoThreadedWork() runs lore commands, fills the "results" fields
//   game thread : provider's Tick() sees bWorkDone, applies results to the
//                 state cache and calls Unreal's completion delegate
//
// The worker never touches the provider or the state cache, so no locks are
// needed around them.

#include "CoreMinimal.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "Misc/IQueuedWork.h"
#include "LoreUtils.h"

#include <atomic>

enum class ELoreCommandKind : uint8
{
	Operation,          // a normal Unreal operation (Connect, CheckOut, ...)
	BackgroundRefresh   // our own periodic refresh of revision numbers + locks
};

class FLoreCommand : public IQueuedWork
{
public:
	FLoreCommand(const FSourceControlOperationRef& InOperation,
		const TArray<FString>& InFiles,
		const FLoreContext& InContext,
		const TSharedRef<FLoreLockStore, ESPMode::ThreadSafe>& InLockStore,
		const FSourceControlOperationComplete& InDelegate,
		ELoreCommandKind InKind = ELoreCommandKind::Operation);

	// IQueuedWork
	virtual void DoThreadedWork() override;
	virtual void Abandon() override;

	// ---- inputs (set on the game thread before queuing) ----
	FSourceControlOperationRef Operation;
	FName OperationName;
	TArray<FString> Files;                 // absolute, normalized
	FLoreContext Ctx;
	TSharedRef<FLoreLockStore, ESPMode::ThreadSafe> LockStore;
	FSourceControlOperationComplete OperationCompleteDelegate;
	ELoreCommandKind Kind;
	FString CheckInMessage;
	bool bSoftRevert = false;
	bool bKeepCheckedOut = false;          // "Keep files checked out" in the Submit dialog
	bool bUpdateHistory = false;           // UpdateStatus: also read file history

	// ---- results (written by the worker) ----
	std::atomic<bool> bWorkDone{ false };
	bool bCommandSuccessful = false;
	TArray<FString> InfoMessages;
	TArray<FString> WarningMessages;
	TArray<FString> ErrorMessages;
	TArray<FLoreFileStatus> Statuses;
	TArray<FString> ScannedDirs;           // absolute dirs that were fully scanned
	bool bHasFullLockMap = false;
	TMap<FString, FLoreLockInfo> FullLockMap; // absolute path -> lock
	bool bHasRepoInfo = false;
	FLoreRepoInfo RepoInfo;
	FString SuccessMessage;
	TMap<FString, TArray<FLoreHistoryEntry>> Histories; // absolute path -> newest first

private:
	bool DoWork();
};
