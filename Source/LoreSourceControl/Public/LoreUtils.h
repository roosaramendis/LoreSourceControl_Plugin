#pragma once

// Shared helpers for the Lore provider:
//  - running the `lore` CLI safely (exit codes, timeouts, no shell injection)
//  - path conversion (absolute <-> repo-relative)
//  - parsers for the text output of `lore status`, `lore lock ...`, etc.
//  - a small persistent list of the locks THIS machine acquired
//
// Everything in here is safe to call from a worker thread.

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

DECLARE_LOG_CATEGORY_EXTERN(LogLoreSC, Log, All);

// Names of the Unreal source control operations we support.
namespace LoreOpNames
{
	static const FName Connect(TEXT("Connect"));
	static const FName UpdateStatus(TEXT("UpdateStatus"));
	static const FName CheckOut(TEXT("CheckOut"));
	static const FName CheckIn(TEXT("CheckIn"));
	static const FName MarkForAdd(TEXT("MarkForAdd"));
	static const FName Delete(TEXT("Delete"));
	static const FName Revert(TEXT("Revert"));
	static const FName Sync(TEXT("Sync"));
}

// Everything a worker thread needs to run lore. Copied by value into each
// command so workers never touch the provider (which lives on the game thread).
struct FLoreContext
{
	FString LoreBinary;      // full path to the lore executable, or "lore"
	FString RepoRoot;        // absolute, no trailing slash (folder that contains .lore)
	FString Identity;        // --identity value
	FString ServerUrl;       // lore://host:port
	FString RepositoryName;  // name on the server
};

struct FLoreResult
{
	bool bLaunched = false;
	bool bTimedOut = false;
	int32 ExitCode = -1;
	FString Output;          // stdout + stderr, ANSI colours removed

	bool Ok() const { return bLaunched && !bTimedOut && ExitCode == 0; }
	FString Describe() const; // human readable error text
};

// What Lore knows about a file's content.
enum class ELoreWorkingState : uint8
{
	Unknown,     // not queried yet
	Outside,     // not inside the Lore repository (e.g. engine content)
	NotTracked,  // inside the repo but not in Lore yet (new file)
	Unchanged,   // tracked, no local changes
	Added,       // marked for add (staged)
	Modified,    // tracked, changed locally
	Deleted      // tracked, deleted locally
};

enum class ELoreLockState : uint8
{
	None,
	Mine,   // checked out by me
	Other   // checked out by someone else
};

struct FLoreLockInfo
{
	ELoreLockState State = ELoreLockState::None;
	FString Owner;           // empty when the server does not know who owns it
};

// Result for one file, produced on a worker thread, applied on the game thread.
struct FLoreFileStatus
{
	FString AbsPath;
	ELoreWorkingState State = ELoreWorkingState::Unknown;
	bool bStaged = false;
	bool bLockKnown = false; // false = lock info was not queried, keep the old value
	ELoreLockState Lock = ELoreLockState::None;
	FString LockOwner;
	bool bIsCurrent = true;
};

// One line from the "Changes ..." sections of `lore status`.
struct FLoreStatusEntry
{
	TCHAR Code = TEXT('M');  // A, M, D, ...
	bool bStaged = false;
	FString RelPath;
};

// One revision of one file, from `lore file history <path> N`.
struct FLoreHistoryEntry
{
	int32 Number = 0;
	FString Signature;   // the revision specifier lore accepts (--revision)
	FString Address;     // content address of the file at this revision (file write --address)
	TCHAR Action = TEXT('M'); // A, M, D ...
	FString Message;
	FString Creator;
	FString Committer;
	FDateTime Date;      // UTC
};

struct FLoreRepoInfo
{
	FString RepoId;
	FString BranchName;
	TOptional<int32> LocalRevision;
	TOptional<int32> RemoteRevision;
	TArray<FLoreStatusEntry> Entries;
};

// Remembers which locks this machine acquired (relative path -> owner text
// the server showed when we took it). Needed because a Lore server without
// user accounts reports every lock owner as "<unknown>".
class FLoreLockStore
{
public:
	void SetFile(const FString& InFilePath);
	bool Find(const FString& RelPath, FString& OutOwner) const;
	void Add(const FString& RelPath, const FString& Owner);
	void Remove(const FString& RelPath);
	TArray<FString> GetAll() const;

private:
	void SaveLocked() const;

	mutable FCriticalSection Mutex;
	TMap<FString, FString> Locks;
	FString FilePath;
};

// Same "path<TAB>text" file format, used for moves recorded from Unreal
// renames: new repo-relative path -> old repo-relative path.
using FLoreMoveStore = FLoreLockStore;

namespace LoreUtils
{
	// Runs `lore <global options> <Args...>` inside WorkingDir (defaults to Ctx.RepoRoot).
	// TimeoutSeconds <= 0 means no timeout.
	FLoreResult Run(const FLoreContext& Ctx, const TArray<FString>& Args, double TimeoutSeconds = 120.0, const FString& WorkingDir = FString());

	FString NormalizePath(const FString& InPath);
	bool ToRelative(const FString& RepoRoot, const FString& AbsPath, FString& OutRelPath);
	FString ToAbsolute(const FString& RepoRoot, const FString& RelPath);
	FString FindRepoRoot(const FString& StartDir);
	FString DefaultLoreBinary();
	FString ReadConfigValue(const FString& RepoRoot, const FString& Key);

	void ParseStatus(const FString& Output, FLoreRepoInfo& OutInfo);
	void ParseLocks(const FString& Output, TMap<FString, FString>& OutLocks); // rel path -> owner ("" = unknown)
	void ParseRepositoryList(const FString& Output, TArray<TPair<FString, FString>>& OutRepos); // name -> id
	TOptional<int32> ParseFirstHistoryRevision(const FString& Output);
	TOptional<int32> ParseCommittedRevision(const FString& Output);
	void ParseFileHistory(const FString& Output, TArray<FLoreHistoryEntry>& OutEntries);
	FDateTime ParseLoreDate(const FString& Text); // "Sat, 17 Oct 2026 09:35:42 +0000" -> UTC
}
