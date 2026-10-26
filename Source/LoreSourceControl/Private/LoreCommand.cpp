#include "LoreCommand.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

// All the "what lore commands do we run for operation X" logic lives here.
// Every function runs on the worker thread.
namespace LoreWork
{
	static constexpr int32 ChunkSize = 50;          // files per lore call
	static constexpr double ShortTimeout = 120.0;   // status, locks, dirty...
	static constexpr double LongTimeout = 3600.0;   // commit, push, sync, scans

	static void AddError(FLoreCommand& C, const FString& Message)
	{
		C.ErrorMessages.Add(Message);
		UE_LOG(LogLoreSC, Warning, TEXT("%s"), *Message);
	}

	static TArray<FString> Slice(const TArray<FString>& Items, int32 Start, int32 Count)
	{
		TArray<FString> Out;
		for (int32 i = Start; i < Start + Count && i < Items.Num(); ++i)
		{
			Out.Add(Items[i]);
		}
		return Out;
	}

	// Runs `lore <Prefix> <items...>` in chunks. If a chunk fails and
	// bRetryIndividually is set, each item is retried alone so one bad file
	// does not fail the others.
	static bool RunChunked(FLoreCommand& C, const TArray<FString>& Prefix, const TArray<FString>& Items,
		double Timeout, bool bRetryIndividually, TArray<FString>* OutFailed = nullptr, FString* OutLastError = nullptr)
	{
		bool bAllOk = true;
		for (int32 Start = 0; Start < Items.Num(); Start += ChunkSize)
		{
			const TArray<FString> Chunk = Slice(Items, Start, ChunkSize);
			TArray<FString> Args = Prefix;
			Args.Append(Chunk);
			const FLoreResult Result = LoreUtils::Run(C.Ctx, Args, Timeout);
			if (Result.Ok())
			{
				continue;
			}
			if (bRetryIndividually && Chunk.Num() > 1)
			{
				for (const FString& Item : Chunk)
				{
					TArray<FString> OneArgs = Prefix;
					OneArgs.Add(Item);
					const FLoreResult One = LoreUtils::Run(C.Ctx, OneArgs, Timeout);
					if (!One.Ok())
					{
						bAllOk = false;
						if (OutFailed) { OutFailed->Add(Item); }
						if (OutLastError) { *OutLastError = One.Describe(); }
					}
				}
			}
			else
			{
				bAllOk = false;
				if (OutFailed) { OutFailed->Append(Chunk); }
				if (OutLastError) { *OutLastError = Result.Describe(); }
			}
		}
		return bAllOk;
	}

	// Splits the command's files into repo-relative files and directories.
	// Files outside the repository get an "Outside" status right away.
	static void SplitFiles(FLoreCommand& C, TArray<FString>& OutRelFiles, TArray<FString>& OutRelDirs)
	{
		for (const FString& Abs : C.Files)
		{
			if (Abs == C.Ctx.RepoRoot)
			{
				OutRelDirs.Add(TEXT("."));
				continue;
			}
			FString Rel;
			if (!LoreUtils::ToRelative(C.Ctx.RepoRoot, Abs, Rel))
			{
				FLoreFileStatus Status;
				Status.AbsPath = Abs;
				Status.State = ELoreWorkingState::Outside;
				Status.bLockKnown = true;
				C.Statuses.Add(Status);
				continue;
			}
			if (FPaths::DirectoryExists(Abs))
			{
				OutRelDirs.Add(Rel);
			}
			else
			{
				OutRelFiles.Add(Rel);
			}
		}
	}

	static ELoreWorkingState StateFromEntries(const TArray<FLoreStatusEntry>* Entries, bool bExists, bool& bOutStaged)
	{
		bOutStaged = false;
		if (!Entries || Entries->Num() == 0)
		{
			return bExists ? ELoreWorkingState::Unchanged : ELoreWorkingState::NotTracked;
		}
		bool bDeleted = false, bStagedAdd = false, bUnstagedAdd = false;
		for (const FLoreStatusEntry& Entry : *Entries)
		{
			bOutStaged |= Entry.bStaged;
			if (Entry.Code == TEXT('D'))
			{
				bDeleted = true;
			}
			else if (Entry.Code == TEXT('A'))
			{
				if (Entry.bStaged) { bStagedAdd = true; } else { bUnstagedAdd = true; }
			}
			else if (Entry.Code == TEXT('?'))
			{
				bUnstagedAdd = true;
			}
			else if (Entry.Code == TEXT('V') && Entry.bStaged)
			{
				bStagedAdd = true; // "V old -> new": moved here, treat like an add (revert = unstage, never reset)
			}
		}
		if (bDeleted) { return ELoreWorkingState::Deleted; }
		if (bStagedAdd) { return ELoreWorkingState::Added; }   // marked for add
		if (bUnstagedAdd) { return ELoreWorkingState::NotTracked; } // new file, not added yet
		return ELoreWorkingState::Modified;
	}

	// Is this lock mine? A server with user accounts tells us the owner; a
	// server without them says "<unknown>" for everyone, so we fall back to
	// the list of locks this machine acquired.
	static ELoreLockState ClassifyLock(FLoreCommand& C, const FString& Rel, const FString& Owner)
	{
		if (!Owner.IsEmpty() && !C.Ctx.Identity.IsEmpty() && Owner.Equals(C.Ctx.Identity, ESearchCase::IgnoreCase))
		{
			return ELoreLockState::Mine;
		}
		FString StoredOwner;
		if (C.LockStore->Find(Rel, StoredOwner) && StoredOwner == Owner)
		{
			return ELoreLockState::Mine;
		}
		return ELoreLockState::Other;
	}

	static void ForgetLostLock(FLoreCommand& C, const FString& Rel)
	{
		FString Owner;
		if (C.LockStore->Find(Rel, Owner))
		{
			C.LockStore->Remove(Rel);
			C.WarningMessages.Add(FString::Printf(
				TEXT("Your check-out of %s is gone on the server (the server may have restarted). Check it out again before editing."), *Rel));
		}
	}

	// `lore lock query` -> lock state of every locked file in the repository.
	static bool QueryAllLocks(FLoreCommand& C)
	{
		const FLoreResult Result = LoreUtils::Run(C.Ctx, { TEXT("lock"), TEXT("query") }, ShortTimeout);
		if (!Result.Ok())
		{
			return false;
		}
		TMap<FString, FString> Locks;
		LoreUtils::ParseLocks(Result.Output, Locks);

		C.bHasFullLockMap = true;
		for (const TPair<FString, FString>& Pair : Locks)
		{
			FLoreLockInfo Info;
			Info.Owner = Pair.Value;
			Info.State = ClassifyLock(C, Pair.Key, Pair.Value);
			C.FullLockMap.Add(LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Pair.Key), Info);
		}
		for (const FString& Rel : C.LockStore->GetAll())
		{
			if (!Locks.Contains(Rel))
			{
				ForgetLostLock(C, Rel);
			}
		}
		return true;
	}

	// When the server is ahead, ask Lore which files changed between our
	// revision and the server's. Those files are "not at head": Unreal then
	// enables Sync for them and shows the out-of-date icon (like Perforce).
	static void UpdateOutOfDate(FLoreCommand& C)
	{
		if (!C.bHasRepoInfo)
		{
			return;
		}
		const FLoreRepoInfo& Info = C.RepoInfo;
		const bool bBehind = Info.LocalRevision.IsSet() && Info.RemoteRevision.IsSet()
			&& Info.RemoteRevision.GetValue() > Info.LocalRevision.GetValue()
			&& !Info.LocalHash.IsEmpty() && !Info.RemoteHash.IsEmpty();
		if (!bBehind)
		{
			C.bHasOutOfDate = true; // up to date: nothing is out of date
			C.OutOfDateFiles.Reset();
			return;
		}
		FLoreResult Diff = LoreUtils::Run(C.Ctx, { TEXT("revision"), TEXT("diff"), Info.LocalHash, TEXT("--target"), Info.RemoteHash }, ShortTimeout);
		if (!Diff.Ok())
		{
			Diff = LoreUtils::Run(C.Ctx, { TEXT("--remote"), TEXT("revision"), TEXT("diff"), Info.LocalHash, TEXT("--target"), Info.RemoteHash }, ShortTimeout);
		}
		if (!Diff.Ok())
		{
			return; // unknown: keep what the editor had
		}
		TArray<FString> Rels;
		LoreUtils::ParseRevisionDiff(Diff.Output, Rels);
		C.bHasOutOfDate = true;
		C.OutOfDateFiles.Reset();
		for (const FString& Rel : Rels)
		{
			C.OutOfDateFiles.Add(LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Rel));
		}
	}

	static bool ReadRepoInfo(FLoreCommand& C, FLoreRepoInfo* OutFullInfo = nullptr)
	{
		const FLoreResult Result = LoreUtils::Run(C.Ctx, { TEXT("status") }, ShortTimeout);
		if (!Result.Ok())
		{
			return false;
		}
		FLoreRepoInfo Info;
		LoreUtils::ParseStatus(Result.Output, Info);
		if (OutFullInfo)
		{
			*OutFullInfo = Info;
		}
		Info.Entries.Reset();
		C.RepoInfo = Info;
		C.bHasRepoInfo = true;
		return true;
	}

	// The core status query for a list of repo-relative files:
	//   1. `lore dirty`  -> tell Lore these files may have changed (Unreal
	//                       writes files directly, so Lore does not know yet)
	//   2. `lore status --check-dirty` -> real A/M/D state, clears false alarms
	//   3. `lore lock status` -> who holds a lock on each file
	static void RefreshFiles(FLoreCommand& C, const TArray<FString>& Rels, TMap<FString, FLoreFileStatus>& Out, bool bQueryLocks)
	{
		if (Rels.Num() == 0)
		{
			return;
		}
		const FString& Root = C.Ctx.RepoRoot;

		TArray<FString> Existing, Missing;
		for (const FString& Rel : Rels)
		{
			if (FPaths::FileExists(LoreUtils::ToAbsolute(Root, Rel))) { Existing.Add(Rel); } else { Missing.Add(Rel); }
		}
		RunChunked(C, { TEXT("dirty") }, Existing, ShortTimeout, true);
		for (const FString& Rel : Missing)
		{
			LoreUtils::Run(C.Ctx, { TEXT("dirty"), Rel }, ShortTimeout); // fails for unknown files, that is fine
		}

		// ---- status ----
		TMap<FString, TArray<FLoreStatusEntry>> EntriesByRel;
		TSet<FString> StatusKnown;
		auto ReadStatus = [&C, &EntriesByRel, &StatusKnown](const TArray<FString>& Items) -> bool
		{
			TArray<FString> Args = { TEXT("status"), TEXT("--check-dirty") };
			Args.Append(Items);
			const FLoreResult Result = LoreUtils::Run(C.Ctx, Args, ShortTimeout);
			if (!Result.Ok())
			{
				return false;
			}
			FLoreRepoInfo Info;
			LoreUtils::ParseStatus(Result.Output, Info);
			for (const FLoreStatusEntry& Entry : Info.Entries)
			{
				EntriesByRel.FindOrAdd(Entry.RelPath).Add(Entry);
			}
			if (!C.bHasRepoInfo)
			{
				Info.Entries.Reset();
				C.RepoInfo = Info;
				C.bHasRepoInfo = true;
			}
			StatusKnown.Append(Items);
			return true;
		};

		for (int32 Start = 0; Start < Rels.Num(); Start += ChunkSize)
		{
			const TArray<FString> Chunk = Slice(Rels, Start, ChunkSize);
			if (ReadStatus(Chunk))
			{
				continue;
			}
			for (const FString& Rel : Chunk)
			{
				if (ReadStatus({ Rel }))
				{
					continue;
				}
				if (!FPaths::FileExists(LoreUtils::ToAbsolute(Root, Rel)))
				{
					StatusKnown.Add(Rel); // missing and unknown to Lore -> just "not tracked"
				}
				else
				{
					AddError(C, FString::Printf(TEXT("Could not read the Lore status of %s."), *Rel));
				}
			}
		}

		// ---- locks ----
		TMap<FString, FString> Locks;
		TSet<FString> LockKnown;
		if (bQueryLocks)
		{
			auto ReadLocks = [&C, &Locks, &LockKnown](const TArray<FString>& Items) -> bool
			{
				TArray<FString> Args = { TEXT("lock"), TEXT("status") };
				Args.Append(Items);
				const FLoreResult Result = LoreUtils::Run(C.Ctx, Args, ShortTimeout);
				if (!Result.Ok())
				{
					return false;
				}
				LoreUtils::ParseLocks(Result.Output, Locks);
				LockKnown.Append(Items);
				return true;
			};
			for (int32 Start = 0; Start < Rels.Num(); Start += ChunkSize)
			{
				const TArray<FString> Chunk = Slice(Rels, Start, ChunkSize);
				if (ReadLocks(Chunk))
				{
					continue;
				}
				for (const FString& Rel : Chunk)
				{
					ReadLocks({ Rel }); // new/untracked files can't be locked; failure just means "unknown"
				}
			}
		}

		// ---- combine ----
		for (const FString& Rel : Rels)
		{
			if (!StatusKnown.Contains(Rel))
			{
				continue; // keep whatever the editor had before
			}
			FLoreFileStatus Status;
			Status.AbsPath = LoreUtils::ToAbsolute(Root, Rel);
			Status.State = StateFromEntries(EntriesByRel.Find(Rel), FPaths::FileExists(Status.AbsPath), Status.bStaged);

			if (Status.State == ELoreWorkingState::NotTracked)
			{
				Status.bLockKnown = true; // can't be locked
			}
			else if (LockKnown.Contains(Rel))
			{
				Status.bLockKnown = true;
				if (const FString* Owner = Locks.Find(Rel))
				{
					Status.LockOwner = *Owner;
					Status.Lock = ClassifyLock(C, Rel, *Owner);
				}
				else
				{
					Status.Lock = ELoreLockState::None;
					ForgetLostLock(C, Rel);
				}
			}
			Out.Add(Rel, Status);
		}
	}

	static void AddStatuses(FLoreCommand& C, const TMap<FString, FLoreFileStatus>& Map)
	{
		for (const TPair<FString, FLoreFileStatus>& Pair : Map)
		{
			C.Statuses.Add(Pair.Value);
		}
	}

	static FString OwnerText(const FString& Owner)
	{
		return Owner.IsEmpty() ? FString(TEXT("another user")) : Owner;
	}

	// ======================================================================
	// Operations
	// ======================================================================

	static bool GatherStatus(FLoreCommand& C)
	{
		TArray<FString> RelFiles, RelDirs;
		SplitFiles(C, RelFiles, RelDirs);
		const FString& Root = C.Ctx.RepoRoot;

		// Directories (the editor asks for Content/, Config/ ... before "Submit
		// Content"): walk the filesystem with --scan to find every change.
		// Unreal lists config files in Submit Content only when revision control
		// already reports them as changed, but it only asks us to scan the
		// content folders. So whenever it asks for a folder scan, scan the
		// project's Config folder too.
		if (RelDirs.Num() > 0 && FPaths::DirectoryExists(Root / TEXT("Config")))
		{
			bool bConfigCovered = false;
			for (const FString& Dir : RelDirs)
			{
				bConfigCovered |= Dir == TEXT(".") || Dir == TEXT("Config") || Dir.StartsWith(TEXT("Config/"));
			}
			if (!bConfigCovered)
			{
				RelDirs.Add(TEXT("Config"));
			}
		}

		if (RelDirs.Num() > 0)
		{
			TArray<FLoreStatusEntry> Entries;
			TArray<FString> Scanned;
			auto Scan = [&C, &Entries, &Scanned](const TArray<FString>& Dirs) -> bool
			{
				TArray<FString> Args = { TEXT("status"), TEXT("--scan") };
				Args.Append(Dirs);
				const FLoreResult Result = LoreUtils::Run(C.Ctx, Args, LongTimeout);
				if (!Result.Ok())
				{
					return false;
				}
				FLoreRepoInfo Info;
				LoreUtils::ParseStatus(Result.Output, Info);
				Entries.Append(Info.Entries);
				Scanned.Append(Dirs);
				Info.Entries.Reset();
				C.RepoInfo = Info;
				C.bHasRepoInfo = true;
				return true;
			};

			// One call for all folders; if Lore rejects one of them (e.g. a
			// plugin folder that isn't in the repository) retry one by one.
			if (!Scan(RelDirs) && RelDirs.Num() > 1)
			{
				for (const FString& Dir : RelDirs)
				{
					if (!Scan({ Dir }))
					{
						UE_LOG(LogLoreSC, Log, TEXT("Skipping folder %s: lore status --scan failed for it."), *Dir);
					}
				}
			}
			if (Scanned.Num() == 0)
			{
				AddError(C, TEXT("lore status --scan failed for: ") + FString::Join(RelDirs, TEXT(", ")));
			}
			else
			{
				QueryAllLocks(C);

				TMap<FString, TArray<FLoreStatusEntry>> ByRel;
				for (const FLoreStatusEntry& Entry : Entries)
				{
					ByRel.FindOrAdd(Entry.RelPath).Add(Entry);
				}
				for (const TPair<FString, TArray<FLoreStatusEntry>>& Pair : ByRel)
				{
					FLoreFileStatus Status;
					Status.AbsPath = LoreUtils::ToAbsolute(Root, Pair.Key);
					if (FPaths::DirectoryExists(Status.AbsPath))
					{
						continue;
					}
					Status.State = StateFromEntries(&Pair.Value, FPaths::FileExists(Status.AbsPath), Status.bStaged);
					if (C.bHasFullLockMap)
					{
						Status.bLockKnown = true;
						if (const FLoreLockInfo* Lock = C.FullLockMap.Find(Status.AbsPath))
						{
							Status.Lock = Lock->State;
							Status.LockOwner = Lock->Owner;
						}
					}
					C.Statuses.Add(Status);
				}
				for (const FString& Dir : Scanned)
				{
					C.ScannedDirs.Add(Dir == TEXT(".") ? Root : LoreUtils::ToAbsolute(Root, Dir));
				}
			}
		}

		if (RelFiles.Num() > 0)
		{
			TMap<FString, FLoreFileStatus> Out;
			RefreshFiles(C, RelFiles, Out, true);
			AddStatuses(C, Out);

			// History window / Diff: one `lore file history` per file.
			if (C.bUpdateHistory)
			{
				for (const FString& Rel : RelFiles)
				{
					const FLoreFileStatus* Status = Out.Find(Rel);
					if (Status && (Status->State == ELoreWorkingState::NotTracked || Status->State == ELoreWorkingState::Added))
					{
						continue; // never committed -> no history
					}
					const FLoreResult History = LoreUtils::Run(C.Ctx, { TEXT("file"), TEXT("history"), Rel, TEXT("100") }, ShortTimeout);
					if (!History.Ok())
					{
						UE_LOG(LogLoreSC, Log, TEXT("No history for %s: %s"), *Rel, *History.Describe());
						continue;
					}
					TArray<FLoreHistoryEntry> Entries;
					LoreUtils::ParseFileHistory(History.Output, Entries);
					C.Histories.Add(LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Rel), MoveTemp(Entries));
				}
			}
		}

		// No files at all: repository-wide refresh (revision numbers + locks).
		if (C.Files.Num() == 0)
		{
			if (!ReadRepoInfo(C))
			{
				AddError(C, TEXT("lore status failed."));
			}
			QueryAllLocks(C);
		}
		return C.ErrorMessages.Num() == 0;
	}

	static bool RunConnect(FLoreCommand& C)
	{
		if (!FPaths::DirectoryExists(C.Ctx.RepoRoot / TEXT(".lore")))
		{
			AddError(C, FString::Printf(TEXT("No Lore repository found in %s. Open the Revision Control settings and use 'Set Up Repository'."), *C.Ctx.RepoRoot));
			return false;
		}
		const FLoreResult Status = LoreUtils::Run(C.Ctx, { TEXT("status") }, ShortTimeout);
		if (!Status.Ok())
		{
			AddError(C, TEXT("lore status failed: ") + Status.Describe());
			return false;
		}
		LoreUtils::ParseStatus(Status.Output, C.RepoInfo);
		C.RepoInfo.Entries.Reset();
		C.bHasRepoInfo = true;

		if (!C.Ctx.ServerUrl.IsEmpty())
		{
			const FLoreResult List = LoreUtils::Run(C.Ctx, { TEXT("repository"), TEXT("list"), C.Ctx.ServerUrl }, 30.0);
			if (!List.Ok())
			{
				AddError(C, FString::Printf(TEXT("Cannot reach the Lore server %s:\n%s"), *C.Ctx.ServerUrl, *List.Describe()));
				return false;
			}
			TArray<TPair<FString, FString>> Repos;
			LoreUtils::ParseRepositoryList(List.Output, Repos);
			bool bFound = false;
			for (const TPair<FString, FString>& Repo : Repos)
			{
				bFound |= Repo.Value.Equals(C.RepoInfo.RepoId, ESearchCase::IgnoreCase);
			}
			if (Repos.Num() > 0 && !bFound)
			{
				AddError(C, FString::Printf(TEXT("The server %s does not have this project's repository (id %s)."), *C.Ctx.ServerUrl, *C.RepoInfo.RepoId));
				return false;
			}
		}
		QueryAllLocks(C);
		UpdateOutOfDate(C);
		return true;
	}

	static bool RunCheckOut(FLoreCommand& C)
	{
		TArray<FString> Rels, Dirs;
		SplitFiles(C, Rels, Dirs);
		TMap<FString, FLoreFileStatus> States;
		RefreshFiles(C, Rels, States, true);
		bool bOk = C.ErrorMessages.Num() == 0;

		TArray<FString> ToLock;
		for (const FString& Rel : Rels)
		{
			const FLoreFileStatus* Status = States.Find(Rel);
			if (!Status)
			{
				bOk = false;
				continue;
			}
			if (Status->State == ELoreWorkingState::NotTracked || Status->State == ELoreWorkingState::Added)
			{
				continue; // new files have nothing to lock yet
			}
			if (Status->Lock == ELoreLockState::Other)
			{
				AddError(C, FString::Printf(TEXT("%s is checked out by %s."), *Rel, *OwnerText(Status->LockOwner)));
				bOk = false;
				continue;
			}
			if (Status->Lock == ELoreLockState::Mine)
			{
				continue;
			}
			ToLock.Add(Rel);
		}

		// Perforce-style "not at head revision" check: don't let anyone lock a
		// file that already has a newer version on the server.
		if (ToLock.Num() > 0 && ToLock.Num() <= 20)
		{
			for (int32 i = ToLock.Num() - 1; i >= 0; --i)
			{
				const FString Rel = ToLock[i];
				const FLoreResult Local = LoreUtils::Run(C.Ctx, { TEXT("file"), TEXT("history"), Rel, TEXT("1"), TEXT("--oneline") }, ShortTimeout);
				const FLoreResult Remote = LoreUtils::Run(C.Ctx, { TEXT("--remote"), TEXT("file"), TEXT("history"), Rel, TEXT("1"), TEXT("--oneline") }, ShortTimeout);
				if (!Local.Ok() || !Remote.Ok())
				{
					continue;
				}
				const TOptional<int32> LocalRev = LoreUtils::ParseFirstHistoryRevision(Local.Output);
				const TOptional<int32> RemoteRev = LoreUtils::ParseFirstHistoryRevision(Remote.Output);
				if (LocalRev.IsSet() && RemoteRev.IsSet() && RemoteRev.GetValue() > LocalRev.GetValue())
				{
					AddError(C, FString::Printf(TEXT("%s has a newer version on the server (revision %d, you have %d). Sync first, then check out."),
						*Rel, RemoteRev.GetValue(), LocalRev.GetValue()));
					States.FindChecked(Rel).bIsCurrent = false;
					ToLock.RemoveAt(i);
					bOk = false;
				}
			}
		}

		if (ToLock.Num() > 0)
		{
			TArray<FString> Failed;
			FString LastError;
			RunChunked(C, { TEXT("lock"), TEXT("acquire") }, ToLock, ShortTimeout, true, &Failed, &LastError);
			for (const FString& Rel : Failed)
			{
				AddError(C, FString::Printf(TEXT("Could not check out %s:\n%s"), *Rel, *LastError));
				ToLock.Remove(Rel);
				bOk = false;
			}

			// Verify the lock really exists, and remember it as ours.
			TMap<FString, FString> Locks;
			for (int32 Start = 0; Start < ToLock.Num(); Start += ChunkSize)
			{
				TArray<FString> Args = { TEXT("lock"), TEXT("status") };
				Args.Append(Slice(ToLock, Start, ChunkSize));
				const FLoreResult Result = LoreUtils::Run(C.Ctx, Args, ShortTimeout);
				if (Result.Ok())
				{
					LoreUtils::ParseLocks(Result.Output, Locks);
				}
			}
			for (const FString& Rel : ToLock)
			{
				if (const FString* Owner = Locks.Find(Rel))
				{
					C.LockStore->Add(Rel, *Owner);
					FLoreFileStatus& Status = States.FindChecked(Rel);
					Status.Lock = ELoreLockState::Mine;
					Status.LockOwner = *Owner;
					Status.bLockKnown = true;
				}
				else
				{
					AddError(C, FString::Printf(TEXT("Lore did not report a lock on %s after checking it out."), *Rel));
					bOk = false;
				}
			}
		}

		AddStatuses(C, States);
		return bOk;
	}

	// Releases locks this machine owns. A lock on a path that is no longer in
	// the repository (deleted, or moved away) can only be released with
	// --force; only used for our own locks, so it never takes someone else's.
	// "Lock does not exist" counts as released. Returns the paths still locked.
	static TArray<FString> ReleaseMyLocks(FLoreCommand& C, const TArray<FString>& Rels)
	{
		TArray<FString> StillLocked;
		for (int32 Start = 0; Start < Rels.Num(); Start += ChunkSize)
		{
			const TArray<FString> Chunk = Slice(Rels, Start, ChunkSize);
			TArray<FString> Args = { TEXT("lock"), TEXT("release") };
			Args.Append(Chunk);
			if (LoreUtils::Run(C.Ctx, Args, ShortTimeout).Ok())
			{
				continue;
			}
			for (const FString& Rel : Chunk)
			{
				const FLoreResult Normal = LoreUtils::Run(C.Ctx, { TEXT("lock"), TEXT("release"), Rel }, ShortTimeout);
				if (Normal.Ok() || Normal.Output.Contains(TEXT("does not exist")))
				{
					continue;
				}
				const FLoreResult Forced = LoreUtils::Run(C.Ctx, { TEXT("lock"), TEXT("release"), TEXT("--force"), Rel }, ShortTimeout);
				if (!Forced.Ok() && !Forced.Output.Contains(TEXT("does not exist")))
				{
					StillLocked.Add(Rel);
				}
			}
		}
		for (const FString& Rel : Rels)
		{
			if (!StillLocked.Contains(Rel))
			{
				C.LockStore->Remove(Rel);
			}
		}
		return StillLocked;
	}

	// Records "OldRel was renamed to NewRel" with `lore stage move`, so the
	// file keeps its history. Lore only records the move: the file must
	// already be at NewRel on disk (Unreal did that). Returns false when Lore
	// refuses (e.g. OldRel was never submitted); the file is then just added.
	static bool StageMove(FLoreCommand& C, const FString& OldRel, const FString& NewRel)
	{
		if (!FPaths::FileExists(LoreUtils::ToAbsolute(C.Ctx.RepoRoot, NewRel)))
		{
			return false;
		}
		// Only a file that is in the current revision can be moved. Lore 0.8.4
		// does not refuse an unknown source path: it silently links the new file
		// to some unrelated deleted file's history. So check first.
		const FLoreResult InfoCheck = LoreUtils::Run(C.Ctx, { TEXT("file"), TEXT("info"), OldRel }, ShortTimeout);
		if (!InfoCheck.Ok() || !InfoCheck.Output.Contains(TEXT("Type:")))
		{
			UE_LOG(LogLoreSC, Log, TEXT("Not recording %s -> %s as a move: %s was never submitted, adding it as a new file."), *OldRel, *NewRel, *OldRel);
			if (C.MoveStore.IsValid())
			{
				C.MoveStore->Remove(NewRel);
			}
			return false;
		}
		const FLoreResult Status = LoreUtils::Run(C.Ctx, { TEXT("status") }, ShortTimeout);
		if (Status.Ok())
		{
			FLoreRepoInfo Info;
			LoreUtils::ParseStatus(Status.Output, Info);
			for (const FLoreStatusEntry& Entry : Info.Entries)
			{
				if (Entry.Code == TEXT('V') && Entry.bStaged && Entry.RelPath == NewRel)
				{
					return true; // already recorded
				}
			}
		}
		const FLoreResult Move = LoreUtils::Run(C.Ctx, { TEXT("stage"), TEXT("move"), OldRel, NewRel }, ShortTimeout);
		if (!Move.Ok())
		{
			UE_LOG(LogLoreSC, Log, TEXT("Not recording %s -> %s as a move: %s"), *OldRel, *NewRel, *Move.Describe());
			return false;
		}
		UE_LOG(LogLoreSC, Log, TEXT("Recorded move %s -> %s"), *OldRel, *NewRel);
		return true;
	}

	static bool RunMarkForAdd(FLoreCommand& C)
	{
		TArray<FString> Rels, Dirs;
		SplitFiles(C, Rels, Dirs);

		TArray<FString> Existing;
		for (const FString& Rel : Rels)
		{
			if (FPaths::FileExists(LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Rel))) { Existing.Add(Rel); }
			else { AddError(C, FString::Printf(TEXT("Cannot mark %s for add: the file does not exist."), *Rel)); }
		}

		// Files Unreal just renamed: record the move first so history follows.
		if (C.MoveStore.IsValid())
		{
			for (const FString& Rel : Existing)
			{
				FString OldRel;
				if (C.MoveStore->Find(Rel, OldRel))
				{
					StageMove(C, OldRel, Rel);
				}
			}
		}

		RunChunked(C, { TEXT("dirty") }, Existing, ShortTimeout, true);
		TArray<FString> Failed;
		FString LastError;
		if (!RunChunked(C, { TEXT("stage") }, Existing, LongTimeout, true, &Failed, &LastError))
		{
			AddError(C, FString::Printf(TEXT("Could not mark for add:\n%s\n%s"), *FString::Join(Failed, TEXT("\n")), *LastError));
		}

		TMap<FString, FLoreFileStatus> After;
		RefreshFiles(C, Rels, After, false);
		AddStatuses(C, After);
		return C.ErrorMessages.Num() == 0;
	}

	static bool RunDelete(FLoreCommand& C)
	{
		TArray<FString> Rels, Dirs;
		SplitFiles(C, Rels, Dirs);
		TMap<FString, FLoreFileStatus> States;
		RefreshFiles(C, Rels, States, true);

		bool bOk = true;
		TArray<FString> Touched;
		for (const FString& Rel : Rels)
		{
			const FLoreFileStatus* Status = States.Find(Rel);
			const FString Abs = LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Rel);
			if (Status && Status->Lock == ELoreLockState::Other)
			{
				AddError(C, FString::Printf(TEXT("Cannot delete %s: it is checked out by %s."), *Rel, *OwnerText(Status->LockOwner)));
				bOk = false;
				continue;
			}
			const bool bTracked = Status && Status->State != ELoreWorkingState::Added && Status->State != ELoreWorkingState::NotTracked;
			if (Status && Status->State == ELoreWorkingState::Added)
			{
				LoreUtils::Run(C.Ctx, { TEXT("unstage"), Rel }, ShortTimeout);
			}
			IFileManager::Get().Delete(*Abs, false, true, true);
			if (bTracked)
			{
				LoreUtils::Run(C.Ctx, { TEXT("dirty"), Rel }, ShortTimeout); // missing tracked file -> marked as delete
			}
			Touched.Add(Rel);
		}

		TMap<FString, FLoreFileStatus> After;
		RefreshFiles(C, Touched, After, true);
		AddStatuses(C, After);
		return bOk;
	}

	static bool RunRevert(FLoreCommand& C)
	{
		TArray<FString> Rels, Dirs;
		SplitFiles(C, Rels, Dirs);
		TMap<FString, FLoreFileStatus> States;
		RefreshFiles(C, Rels, States, true);

		TArray<FString> ToReset, ToRelease, ToUnstage;
		for (const FString& Rel : Rels)
		{
			const FLoreFileStatus* Status = States.Find(Rel);
			const ELoreWorkingState State = Status ? Status->State : ELoreWorkingState::Unknown;

			// A renamed asset: never reset (that could delete it), only unstage,
			// and tell the user how to undo a rename.
			FString RenamedFrom;
			if (C.MoveStore.IsValid() && C.MoveStore->Find(Rel, RenamedFrom))
			{
				ToUnstage.Add(Rel);
				C.WarningMessages.Add(FString::Printf(
					TEXT("%s was renamed from %s. Revert does not undo a rename. To undo it: right-click the folder -> Fix Up Redirectors, then rename the asset back to %s (the recorded move is then cancelled)."),
					*FPaths::GetBaseFilename(Rel), *FPaths::GetBaseFilename(RenamedFrom), *FPaths::GetBaseFilename(RenamedFrom)));
				continue;
			}

			if (State == ELoreWorkingState::Modified || State == ELoreWorkingState::Deleted)
			{
				ToReset.Add(Rel);
			}
			if (Status && (Status->bStaged || State == ELoreWorkingState::Added))
			{
				ToUnstage.Add(Rel); // "Added" files are only unstaged: the file itself is kept
			}
			FString StoredOwner;
			if ((Status && Status->Lock == ELoreLockState::Mine) || C.LockStore->Find(Rel, StoredOwner))
			{
				ToRelease.Add(Rel);
			}
		}

		if (ToUnstage.Num() > 0)
		{
			RunChunked(C, { TEXT("unstage") }, ToUnstage, ShortTimeout, true);
		}

		bool bOk = true;
		if (!C.bSoftRevert && ToReset.Num() > 0)
		{
			TArray<FString> Failed;
			FString LastError;
			if (!RunChunked(C, { TEXT("reset") }, ToReset, LongTimeout, true, &Failed, &LastError))
			{
				AddError(C, FString::Printf(TEXT("Could not revert:\n%s\n%s"), *FString::Join(Failed, TEXT("\n")), *LastError));
				bOk = false;
			}
		}

		if (ToRelease.Num() > 0)
		{
			const TArray<FString> Failed = ReleaseMyLocks(C, ToRelease);
			if (Failed.Num() > 0)
			{
				C.WarningMessages.Add(TEXT("Reverted, but could not release the check-out of: ") + FString::Join(Failed, TEXT(", ")));
			}
		}

		TMap<FString, FLoreFileStatus> After;
		RefreshFiles(C, Rels, After, true);
		AddStatuses(C, After);
		return bOk;
	}

	// "[Error] File has local changes: Config/X.ini (incoming size ...)" -> paths.
	static TArray<FString> ParseSyncBlockers(const FString& Output)
	{
		static const FString Marker = TEXT("File has local changes: ");
		TArray<FString> Lines, Blockers;
		Output.ParseIntoArrayLines(Lines, true);
		for (const FString& Line : Lines)
		{
			const int32 Start = Line.Find(Marker, ESearchCase::CaseSensitive);
			if (Start == INDEX_NONE)
			{
				continue;
			}
			FString Path = Line.Mid(Start + Marker.Len());
			const int32 Paren = Path.Find(TEXT(" ("), ESearchCase::CaseSensitive);
			if (Paren != INDEX_NONE)
			{
				Path.LeftInline(Paren);
			}
			Blockers.AddUnique(Path.TrimStartAndEnd());
		}
		return Blockers;
	}

	static FString DescribeSyncFailure(const FLoreResult& Result)
	{
		const TArray<FString> Blockers = ParseSyncBlockers(Result.Output);
		if (Blockers.Num() > 0)
		{
			return TEXT("These files were changed on the server AND you have local changes to them:\n  ") + FString::Join(Blockers, TEXT("\n  "))
				+ TEXT("\nNothing was changed. Run Sync again: text files are merged automatically, and for assets you will be asked whether to keep your version or take theirs.")
				+ TEXT("\n(If you deleted one of these files, Revert the delete first.)");
		}
		return Result.Describe();
	}

	// A small FLoreResult for a failure that happened before lore ran.
	static FLoreResult MakeFailure(const FString& Message)
	{
		FLoreResult Result;
		Result.bLaunched = true;
		Result.ExitCode = 1;
		Result.Output = Message;
		return Result;
	}

	// `lore sync` stopped because of local changes (exit 39). Do what Perforce's
	// "sync + resolve" does, for exactly the files Lore listed:
	//   1. back up my version              -> Saved/LoreSourceControl/SyncBackups/<time>/<path>
	//   2. `lore file reset` those files   -> the version we both started from (kept as <path>.base)
	//   3. `lore sync`                     -> their version
	//   4. text:  diff3 merges mine + theirs.  assets: keep mine or keep theirs (user chose earlier)
	// My version is never deleted: if anything fails it is copied back.
	static FLoreResult SyncResolvingLocalChanges(FLoreCommand& C, const TArray<FString>& Blockers, const FLoreResult& FirstAttempt)
	{
		struct FItem
		{
			FString Rel, Abs, Mine, Base;
			bool bText = false;
			bool bKeepMine = false;
		};

		// 1. Plan. If any file has no answer yet, change nothing.
		TArray<FItem> Items;
		TArray<FString> Undecided;
		for (const FString& Rel : Blockers)
		{
			FItem Item;
			Item.Rel = Rel;
			Item.Abs = LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Rel);
			Item.bText = LoreUtils::IsMergeableText(Rel);
			Item.bKeepMine = C.ResolveKeepMine.Contains(Item.Abs);
			const bool bDecided = Item.bText || Item.bKeepMine || C.ResolveTakeTheirs.Contains(Item.Abs);
			if (!bDecided || !FPaths::FileExists(Item.Abs))
			{
				Undecided.Add(Rel);
				continue;
			}
			Items.Add(Item);
		}
		if (Undecided.Num() > 0 || Items.Num() == 0)
		{
			return FirstAttempt; // RunSync explains which files and what to do
		}

		// From here on the user's files are being changed: finish no matter what.
		const std::atomic<bool>* SavedFlag = C.Ctx.CancelFlag;
		C.Ctx.CancelFlag = nullptr;
		C.bPastPointOfNoReturn = true;

		const FString BackupDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())
			/ TEXT("LoreSourceControl/SyncBackups") / FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
		IFileManager& FM = IFileManager::Get();

		auto PutMineBack = [&C, &FM](const TArray<FItem>& ToRestore)
		{
			TArray<FString> RestoredRels;
			for (const FItem& Item : ToRestore)
			{
				if (FM.Copy(*Item.Abs, *Item.Mine, true, true) != COPY_OK)
				{
					AddError(C, FString::Printf(TEXT("Could not put your version of %s back. It is saved here:\n  %s"), *Item.Rel, *Item.Mine));
				}
				RestoredRels.Add(Item.Rel);
			}
			RunChunked(C, { TEXT("dirty") }, RestoredRels, ShortTimeout, true);
		};

		// 2. Back up my version.
		TArray<FString> Rels;
		for (FItem& Item : Items)
		{
			Item.Mine = BackupDir / Item.Rel;
			Item.Base = Item.Mine + TEXT(".base");
			FM.MakeDirectory(*FPaths::GetPath(Item.Mine), true);
			if (FM.Copy(*Item.Mine, *Item.Abs, true, true) != COPY_OK)
			{
				C.Ctx.CancelFlag = SavedFlag;
				return MakeFailure(FString::Printf(TEXT("Could not back up %s to %s, so nothing was changed."), *Item.Rel, *Item.Mine));
			}
			Rels.Add(Item.Rel);
		}
		UE_LOG(LogLoreSC, Log, TEXT("Sync: backed up %d locally changed file(s) to %s"), Items.Num(), *BackupDir);

		// 3. Throw away the local change of just these files (backed up above).
		FString ResetError;
		if (!RunChunked(C, { TEXT("file"), TEXT("reset") }, Rels, ShortTimeout, false, nullptr, &ResetError))
		{
			PutMineBack(Items);
			C.Ctx.CancelFlag = SavedFlag;
			return MakeFailure(TEXT("Could not set your changes aside (lore file reset failed), so your files were left as they were.\n") + ResetError);
		}
		for (FItem& Item : Items)
		{
			if (Item.bText)
			{
				FM.Copy(*Item.Base, *Item.Abs, true, true); // the version we both started from
			}
		}

		// 4. Sync. Now nothing blocks it.
		const FLoreResult Second = LoreUtils::Run(C.Ctx, { TEXT("sync") }, LongTimeout);
		if (!Second.Ok())
		{
			PutMineBack(Items);
			C.WarningMessages.Add(TEXT("The sync failed, so your versions were put back. Backups: ") + BackupDir);
			C.Ctx.CancelFlag = SavedFlag;
			return Second;
		}

		// 5. Combine: the file on disk is now their version.
		TArray<FString> Merged, Conflicts, KeptMine, TookTheirs, NotMerged, NeedDirty;
		bool bConfigChanged = false;
		for (const FItem& Item : Items)
		{
			bConfigChanged |= FPaths::GetExtension(Item.Rel).Equals(TEXT("ini"), ESearchCase::IgnoreCase);
			if (Item.bText)
			{
				const FString MergedFile = Item.Mine + TEXT(".merged");
				FString MergeMessage;
				const int32 Code = FM.FileExists(*Item.Base)
					? LoreUtils::Merge3(Item.Mine, Item.Base, Item.Abs, MergedFile, MergeMessage)
					: -1;
				if ((Code == 0 || Code == 1) && FM.Copy(*Item.Abs, *MergedFile, true, true) == COPY_OK)
				{
					(Code == 0 ? Merged : Conflicts).Add(Item.Rel);
					NeedDirty.Add(Item.Rel);
				}
				else
				{
					NotMerged.Add(FString::Printf(TEXT("%s  (yours: %s)%s"), *Item.Rel, *Item.Mine,
						MergeMessage.IsEmpty() ? TEXT("") : *(TEXT("  ") + MergeMessage)));
				}
			}
			else if (Item.bKeepMine)
			{
				if (FM.Copy(*Item.Abs, *Item.Mine, true, true) == COPY_OK)
				{
					KeptMine.Add(Item.Rel);
					NeedDirty.Add(Item.Rel);
				}
				else
				{
					NotMerged.Add(FString::Printf(TEXT("%s  (yours: %s)"), *Item.Rel, *Item.Mine));
				}
			}
			else
			{
				TookTheirs.Add(FString::Printf(TEXT("%s  (yours: %s)"), *Item.Rel, *Item.Mine));
			}
		}
		if (NeedDirty.Num() > 0)
		{
			RunChunked(C, { TEXT("dirty") }, NeedDirty, ShortTimeout, true);
		}

		// 6. Tell the user exactly what happened.
		FString Report;
		auto Section = [&Report](const TArray<FString>& List, const TCHAR* Title)
		{
			if (List.Num() > 0)
			{
				Report += FString(Title) + TEXT("\n  ") + FString::Join(List, TEXT("\n  ")) + TEXT("\n");
			}
		};
		Section(Merged, TEXT("Merged your changes with theirs:"));
		Section(Conflicts, TEXT("CONFLICT - you both changed the same lines. Open these files and fix the parts between <<<<<<< and >>>>>>> before submitting:"));
		Section(KeptMine, TEXT("Kept your version (submitting it replaces theirs):"));
		Section(TookTheirs, TEXT("Took their version (yours is backed up):"));
		Section(NotMerged, TEXT("Could not merge, so their version is on disk now (yours is backed up):"));
		if (bConfigChanged)
		{
			Report += TEXT("Config files changed: restart the editor so it reads them.\n");
		}
		Report += TEXT("Backups of your versions: ") + BackupDir;
		C.WarningMessages.Add(Report);

		C.Ctx.CancelFlag = SavedFlag;
		return Second;
	}

	// `lore sync` refuses while anything is staged ("Unable to sync when there
	// is a staged state"), so set staged files aside, sync, and stage them
	// again afterwards (moves are recorded again with `stage move`).
	// bResolveLocalChanges: if local edits block the sync, merge them (see above).
	static FLoreResult SyncPreservingStage(FLoreCommand& C, bool bResolveLocalChanges = false)
	{
		TArray<FString> Staged;
		const FLoreResult Status = LoreUtils::Run(C.Ctx, { TEXT("status") }, ShortTimeout);
		if (Status.Ok())
		{
			FLoreRepoInfo Info;
			LoreUtils::ParseStatus(Status.Output, Info);
			for (const FLoreStatusEntry& Entry : Info.Entries)
			{
				if (Entry.bStaged && !FPaths::DirectoryExists(LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Entry.RelPath)))
				{
					Staged.AddUnique(Entry.RelPath);
				}
			}
		}
		if (Staged.Num() > 0)
		{
			RunChunked(C, { TEXT("unstage") }, Staged, ShortTimeout, true);
		}

		FLoreResult SyncResult = LoreUtils::Run(C.Ctx, { TEXT("sync") }, LongTimeout);
		if (bResolveLocalChanges && !SyncResult.Ok() && !SyncResult.bCancelled)
		{
			const TArray<FString> Blockers = ParseSyncBlockers(SyncResult.Output);
			if (Blockers.Num() > 0)
			{
				SyncResult = SyncResolvingLocalChanges(C, Blockers, SyncResult);
			}
		}

		if (Staged.Num() > 0)
		{
			// Must run even if the sync was cancelled.
			const std::atomic<bool>* SavedFlag = C.Ctx.CancelFlag;
			C.Ctx.CancelFlag = nullptr;
			TArray<FString> Plain;
			for (const FString& Rel : Staged)
			{
				FString OldRel;
				if (C.MoveStore.IsValid() && C.MoveStore->Find(Rel, OldRel))
				{
					StageMove(C, OldRel, Rel);
				}
				Plain.Add(Rel);
			}
			RunChunked(C, { TEXT("dirty") }, Plain, ShortTimeout, true);
			RunChunked(C, { TEXT("stage") }, Plain, LongTimeout, true);
			C.Ctx.CancelFlag = SavedFlag;
		}
		return SyncResult;
	}

	static bool RunCheckIn(FLoreCommand& C)
	{
		TArray<FString> Rels, Dirs;
		SplitFiles(C, Rels, Dirs);
		TMap<FString, FLoreFileStatus> States;
		RefreshFiles(C, Rels, States, true);
		if (C.ErrorMessages.Num() > 0)
		{
			return false;
		}

		FLoreRepoInfo Repo;
		if (!ReadRepoInfo(C, &Repo))
		{
			AddError(C, TEXT("lore status failed, nothing was submitted."));
			return false;
		}

		// 1. Never submit over someone else's lock.
		TArray<FString> Blocked, ToCommit;
		for (const FString& Rel : Rels)
		{
			const FLoreFileStatus* Status = States.Find(Rel);
			if (!Status)
			{
				continue;
			}
			if (Status->Lock == ELoreLockState::Other && Status->State != ELoreWorkingState::Added)
			{
				Blocked.Add(FString::Printf(TEXT("%s (checked out by %s)"), *Rel, *OwnerText(Status->LockOwner)));
				continue;
			}
			if (Status->State == ELoreWorkingState::Modified || Status->State == ELoreWorkingState::Added
				|| Status->State == ELoreWorkingState::Deleted || Status->State == ELoreWorkingState::NotTracked)
			{
				ToCommit.Add(Rel);
			}
		}
		if (Blocked.Num() > 0)
		{
			AddError(C, TEXT("Nothing was submitted. These files are checked out by someone else:\n") + FString::Join(Blocked, TEXT("\n")));
			return false;
		}

		const bool bLocalAhead = Repo.LocalRevision.IsSet() && Repo.RemoteRevision.IsSet()
			&& Repo.LocalRevision.GetValue() > Repo.RemoteRevision.GetValue();
		if (ToCommit.Num() == 0 && !bLocalAhead)
		{
			AddError(C, TEXT("Nothing to submit: the selected files have no changes."));
			return false;
		}

		// 2. `lore commit` takes EVERYTHING that is staged, so temporarily
		//    unstage files the user did not select in the Submit dialog.
		TSet<FString> Selected;
		Selected.Append(Rels);
		TArray<FString> ToUnstage;
		for (const FLoreStatusEntry& Entry : Repo.Entries)
		{
			if (Entry.bStaged && !Selected.Contains(Entry.RelPath)
				&& !FPaths::DirectoryExists(LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Entry.RelPath)))
			{
				ToUnstage.AddUnique(Entry.RelPath);
			}
		}
		if (ToUnstage.Num() > 0)
		{
			RunChunked(C, { TEXT("unstage") }, ToUnstage, ShortTimeout, true);
		}
		auto Restage = [&C, &ToUnstage]()
		{
			if (ToUnstage.Num() > 0)
			{
				RunChunked(C, { TEXT("stage") }, ToUnstage, LongTimeout, true);
			}
		};

		// 3a. Renames recorded from Unreal: tell Lore they are moves so the
		//     history follows the asset. If the old file is gone (no
		//     redirector left), the move already covers its deletion.
		TArray<FString> MovedNew;
		TSet<FString> MovedAwayOld;
		if (C.MoveStore.IsValid())
		{
			for (const FString& Rel : ToCommit)
			{
				FString OldRel;
				if (C.MoveStore->Find(Rel, OldRel) && StageMove(C, OldRel, Rel))
				{
					MovedNew.Add(Rel);
					if (!FPaths::FileExists(LoreUtils::ToAbsolute(C.Ctx.RepoRoot, OldRel)))
					{
						MovedAwayOld.Add(OldRel);
					}
				}
			}
			ToCommit.RemoveAll([&MovedAwayOld](const FString& Rel) { return MovedAwayOld.Contains(Rel); });
		}

		// 3b. Stage the selected files and commit.
		int32 NewRevision = INDEX_NONE;
		if (ToCommit.Num() > 0)
		{
			TArray<FString> Failed;
			FString LastError;
			if (!RunChunked(C, { TEXT("stage") }, ToCommit, LongTimeout, true, &Failed, &LastError))
			{
				AddError(C, FString::Printf(TEXT("Nothing was submitted. Could not stage:\n%s\n%s"), *FString::Join(Failed, TEXT("\n")), *LastError));
				C.Ctx.CancelFlag = nullptr; // clean-up must run even after Cancel
				Restage();
				return false;
			}
			const FString Message = C.CheckInMessage.TrimStartAndEnd().IsEmpty() ? FString(TEXT("Submitted from Unreal Editor")) : C.CheckInMessage;

			// Last chance to cancel. From here on (commit + push + lock release)
			// Cancel is ignored, so a submit can never stop half way.
			if (C.bCancelRequested)
			{
				C.Ctx.CancelFlag = nullptr; // so the clean-up below is not cancelled too
				Restage();
				return false;
			}
			C.bPastPointOfNoReturn = true;
			C.Ctx.CancelFlag = nullptr;
			UE_LOG(LogLoreSC, Log, TEXT("Submit: committing %d file(s)..."), ToCommit.Num());
			const FLoreResult Commit = LoreUtils::Run(C.Ctx, { TEXT("commit"), Message }, LongTimeout);
			if (!Commit.Ok())
			{
				AddError(C, TEXT("lore commit failed, nothing was submitted:\n") + Commit.Describe());
				Restage();
				return false;
			}
			const TOptional<int32> Revision = LoreUtils::ParseCommittedRevision(Commit.Output);
			if (Revision.IsSet())
			{
				NewRevision = Revision.GetValue();
			}
		}

		// 4. Push. If the server moved on, let it fast-forward merge when
		//    the changes don't overlap.
		FLoreResult Push = LoreUtils::Run(C.Ctx, { TEXT("push"), TEXT("--fast-forward-merge") }, LongTimeout);
		FString PushProblem;
		if (!Push.Ok() && Push.Output.Contains(TEXT("Fast-forward merge has conflicts")))
		{
			// Someone else changed some of the same files. Merge their changes
			// into ours locally (Lore does a 3-way merge), then push again.
			UE_LOG(LogLoreSC, Log, TEXT("Submit: the server has changes to the same files, merging them in..."));
			const FLoreResult Merge = SyncPreservingStage(C);
			if (Merge.Ok())
			{
				Push = LoreUtils::Run(C.Ctx, { TEXT("push"), TEXT("--fast-forward-merge") }, LongTimeout);
				if (Push.Ok())
				{
					C.WarningMessages.Add(TEXT("Someone else had changed some of the same files. Lore merged their changes into yours before submitting. If any of those files are open in the editor, reload them."));
				}
			}
			else
			{
				PushProblem = TEXT("Someone else changed some of the same files, and merging their changes failed:\n") + DescribeSyncFailure(Merge);
			}
		}
		if (!Push.Ok())
		{
			const FString Committed = NewRevision != INDEX_NONE ? FString::Printf(TEXT(" as revision %d"), NewRevision) : FString();
			AddError(C, FString::Printf(TEXT("Your changes were committed locally%s, but uploading to the server failed. Your check-outs are kept.\nFix the problem below, then Submit again to upload the pending commit.\n\n%s"),
				*Committed, PushProblem.IsEmpty() ? *Push.Describe() : *PushProblem));
			Restage();
			TMap<FString, FLoreFileStatus> After;
			RefreshFiles(C, Rels, After, true);
			AddStatuses(C, After);
			ReadRepoInfo(C);
			return false;
		}

		// 4b. The server merged our commit with newer revisions. Update to that
		//     merged revision now: otherwise the next commit fails ("branch has
		//     been advanced") and a later push would upload this commit twice.
		if (Push.Output.Contains(TEXT("fast-forward merged on server")))
		{
			const FLoreResult Update = SyncPreservingStage(C);
			if (!Update.Ok())
			{
				C.WarningMessages.Add(TEXT("Submitted (merged with newer changes on the server), but could not update to the merged revision. Run Sync.\n") + DescribeSyncFailure(Update));
			}
		}

		// 5. Only now release the locks (if this order were reversed, a failed
		//    push would leave the files unprotected).
		TArray<FString> ToRelease;
		for (const FString& Rel : C.bKeepCheckedOut ? TArray<FString>() : Rels)
		{
			const FLoreFileStatus* Status = States.Find(Rel);
			FString StoredOwner;
			if ((Status && Status->Lock == ELoreLockState::Mine) || C.LockStore->Find(Rel, StoredOwner))
			{
				ToRelease.Add(Rel);
			}
		}
		if (ToRelease.Num() > 0)
		{
			const TArray<FString> Failed = ReleaseMyLocks(C, ToRelease);
			if (Failed.Num() > 0)
			{
				C.WarningMessages.Add(TEXT("Submitted, but could not release the check-out of: ") + FString::Join(Failed, TEXT(", ")));
			}
		}
		// The lock stays on the OLD path after a move (Lore does not move it).
		TArray<FString> OldLocks;
		for (const FString& OldRel : MovedAwayOld)
		{
			FString StoredOwner;
			if (C.LockStore->Find(OldRel, StoredOwner) && !ToRelease.Contains(OldRel))
			{
				OldLocks.Add(OldRel);
			}
		}
		if (OldLocks.Num() > 0)
		{
			ReleaseMyLocks(C, OldLocks);
		}
		for (const FString& Rel : MovedNew)
		{
			C.MoveStore->Remove(Rel); // submitted: the move is now part of the history
		}

		Restage();
		TMap<FString, FLoreFileStatus> After;
		RefreshFiles(C, Rels, After, true);
		AddStatuses(C, After);
		ReadRepoInfo(C);
		UpdateOutOfDate(C);

		C.SuccessMessage = NewRevision != INDEX_NONE
			? FString::Printf(TEXT("Submitted revision %d to Lore."), NewRevision)
			: FString(TEXT("Uploaded pending commits to Lore."));
		return true;
	}

	static bool RunSync(FLoreCommand& C)
	{
		UE_LOG(LogLoreSC, Log, TEXT("Sync: downloading the latest revision..."));
		const FLoreResult Result = SyncPreservingStage(C, /*bResolveLocalChanges=*/ true);
		if (Result.bCancelled)
		{
			// Refresh what we can without the cancel flag, so icons are right.
			C.Ctx.CancelFlag = nullptr;
			GatherStatus(C);
			return false;
		}
		if (!Result.Ok())
		{
			AddError(C, TEXT("Sync did not finish.\n") + DescribeSyncFailure(Result));
		}
		else
		{
			C.InfoMessages.Add(TEXT("Synced to the latest revision. (Lore always syncs the whole branch, not single files.)"));

			// A Submit whose upload failed earlier leaves a commit that only
			// exists here (the sync just merged it with the server). Upload it.
			const FLoreResult Upload = LoreUtils::Run(C.Ctx, { TEXT("push"), TEXT("--fast-forward-merge") }, LongTimeout);
			if (Upload.Ok() && Upload.Output.Contains(TEXT("Pushed revision")))
			{
				C.WarningMessages.Add(TEXT("Uploaded a submit that had not reached the server yet (it was merged with the latest changes)."));
				if (Upload.Output.Contains(TEXT("fast-forward merged on server")))
				{
					SyncPreservingStage(C);
				}
			}
		}
		GatherStatus(C);
		ReadRepoInfo(C);
		UpdateOutOfDate(C);
		return Result.Ok();
	}

	// Rollback: put an old revision of each file on disk as a local change.
	// Submitting it afterwards makes that old content the newest revision;
	// Revert brings the current revision back.
	static bool RunSyncToRevision(FLoreCommand& C)
	{
		TArray<FString> Rels, Dirs;
		SplitFiles(C, Rels, Dirs);
		if (Dirs.Num() > 0)
		{
			AddError(C, TEXT("Restoring a revision only works on files, not folders."));
			return false;
		}
		TMap<FString, FLoreFileStatus> States;
		RefreshFiles(C, Rels, States, true);
		if (C.ErrorMessages.Num() > 0)
		{
			return false;
		}

		const bool bIsNumber = C.SyncRevision.IsNumeric();
		bool bOk = true;
		for (const FString& Rel : Rels)
		{
			const FLoreFileStatus* Status = States.Find(Rel);
			if (!Status || Status->State == ELoreWorkingState::NotTracked || Status->State == ELoreWorkingState::Added)
			{
				AddError(C, FString::Printf(TEXT("%s has no submitted revisions to restore."), *Rel));
				bOk = false;
				continue;
			}
			if (Status->Lock == ELoreLockState::Other)
			{
				AddError(C, FString::Printf(TEXT("Cannot restore %s: it is checked out by %s."), *Rel, *OwnerText(Status->LockOwner)));
				bOk = false;
				continue;
			}

			// 1. Find the revision (and make sure we are at the newest one, so
			//    the restored content is submitted on top of it).
			const FLoreResult History = LoreUtils::Run(C.Ctx, { TEXT("file"), TEXT("history"), Rel, TEXT("1000") }, ShortTimeout);
			TArray<FLoreHistoryEntry> Entries;
			if (History.Ok())
			{
				LoreUtils::ParseFileHistory(History.Output, Entries);
			}
			const FLoreHistoryEntry* Target = nullptr;
			for (const FLoreHistoryEntry& Entry : Entries)
			{
				if ((bIsNumber && Entry.Number == FCString::Atoi(*C.SyncRevision)) || (!bIsNumber && Entry.Signature == C.SyncRevision))
				{
					Target = &Entry;
					break;
				}
			}
			if (!Target)
			{
				AddError(C, FString::Printf(TEXT("Revision %s of %s was not found in its history."), *C.SyncRevision, *Rel));
				bOk = false;
				continue;
			}
			const FLoreResult Remote = LoreUtils::Run(C.Ctx, { TEXT("--remote"), TEXT("file"), TEXT("history"), Rel, TEXT("1"), TEXT("--oneline") }, ShortTimeout);
			if (Remote.Ok() && Entries.Num() > 0)
			{
				const TOptional<int32> RemoteRev = LoreUtils::ParseFirstHistoryRevision(Remote.Output);
				if (RemoteRev.IsSet() && RemoteRev.GetValue() > Entries[0].Number)
				{
					AddError(C, FString::Printf(TEXT("%s has a newer version on the server (revision %d). Sync first, then restore."), *Rel, RemoteRev.GetValue()));
					bOk = false;
					continue;
				}
			}

			// 2. Check it out (a restore is an edit like any other).
			if (Status->Lock == ELoreLockState::None)
			{
				const FLoreResult Acquire = LoreUtils::Run(C.Ctx, { TEXT("lock"), TEXT("acquire"), Rel }, ShortTimeout);
				const FLoreResult LockStatus = LoreUtils::Run(C.Ctx, { TEXT("lock"), TEXT("status"), Rel }, ShortTimeout);
				TMap<FString, FString> Locks;
				LoreUtils::ParseLocks(LockStatus.Output, Locks);
				const FString* Owner = Locks.Find(Rel);
				if (!Acquire.Ok() || !Owner)
				{
					AddError(C, FString::Printf(TEXT("Could not check out %s:\n%s"), *Rel, *Acquire.Describe()));
					bOk = false;
					continue;
				}
				C.LockStore->Add(Rel, *Owner);
			}

			// 3. Lore refuses to write inside the repository, so extract to the
			//    temp folder first, then copy over the real file.
			const FString TempFile = FPaths::Combine(FString(FPlatformProcess::UserTempDir()),
				FString::Printf(TEXT("lore_restore_%s%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits), *FPaths::GetExtension(Rel, true)));
			TArray<FString> WriteArgs = { TEXT("file"), TEXT("write") };
			if (!Target->Address.IsEmpty())
			{
				WriteArgs.Append({ TEXT("--address"), Target->Address });
			}
			else
			{
				WriteArgs.Append({ TEXT("--path"), Rel, TEXT("--revision"), Target->Signature });
			}
			WriteArgs.Append({ TEXT("--output"), TempFile });
			const FLoreResult Write = LoreUtils::Run(C.Ctx, WriteArgs, LongTimeout);
			const FString Abs = LoreUtils::ToAbsolute(C.Ctx.RepoRoot, Rel);
			const bool bCopied = Write.Ok() && IFileManager::Get().Copy(*Abs, *TempFile, true, true) == COPY_OK;
			IFileManager::Get().Delete(*TempFile, false, true, true);
			if (!bCopied)
			{
				AddError(C, FString::Printf(TEXT("Could not restore revision %d of %s:\n%s"), Target->Number, *Rel, *Write.Describe()));
				bOk = false;
				continue;
			}
			LoreUtils::Run(C.Ctx, { TEXT("dirty"), Rel }, ShortTimeout);
			C.InfoMessages.Add(FString::Printf(TEXT("Restored revision %d of %s. Submit to make it the latest revision, or Revert to undo."), Target->Number, *Rel));
		}

		TMap<FString, FLoreFileStatus> After;
		RefreshFiles(C, Rels, After, true);
		AddStatuses(C, After);
		return bOk;
	}

	static bool RunBackgroundRefresh(FLoreCommand& C)
	{
		const bool bOk = ReadRepoInfo(C);
		QueryAllLocks(C);
		UpdateOutOfDate(C);
		return bOk;
	}
}

// ---------------------------------------------------------------------------

FLoreCommand::FLoreCommand(const FSourceControlOperationRef& InOperation,
	const TArray<FString>& InFiles,
	const FLoreContext& InContext,
	const TSharedRef<FLoreLockStore, ESPMode::ThreadSafe>& InLockStore,
	const FSourceControlOperationComplete& InDelegate,
	ELoreCommandKind InKind)
	: Operation(InOperation)
	, OperationName(InOperation->GetName())
	, Files(InFiles)
	, Ctx(InContext)
	, LockStore(InLockStore)
	, OperationCompleteDelegate(InDelegate)
	, Kind(InKind)
{
	Ctx.CancelFlag = &bCancelRequested;
}

bool FLoreCommand::IsCancellable() const
{
	if (Kind != ELoreCommandKind::Operation || bPastPointOfNoReturn || bWorkDone)
	{
		return false;
	}
	return (OperationName == LoreOpNames::Sync && SyncRevision.IsEmpty())
		|| OperationName == LoreOpNames::CheckIn
		|| OperationName == LoreOpNames::UpdateStatus
		|| OperationName == LoreOpNames::CheckOut
		|| OperationName == LoreOpNames::Connect;
}

void FLoreCommand::DoThreadedWork()
{
	if (bCancelRequested)
	{
		bCommandSuccessful = false; // cancelled while waiting in the queue
	}
	else
	{
		bCommandSuccessful = DoWork();
	}
	bWorkDone = true;
}

void FLoreCommand::Abandon()
{
	bCommandSuccessful = false;
	ErrorMessages.Add(TEXT("The Lore command was cancelled."));
	bWorkDone = true;
}

bool FLoreCommand::DoWork()
{
	using namespace LoreWork;

	if (Kind == ELoreCommandKind::BackgroundRefresh) { return RunBackgroundRefresh(*this); }
	if (OperationName == LoreOpNames::Connect)       { return RunConnect(*this); }
	if (OperationName == LoreOpNames::UpdateStatus)  { return GatherStatus(*this); }
	if (OperationName == LoreOpNames::CheckOut)      { return RunCheckOut(*this); }
	if (OperationName == LoreOpNames::CheckIn)       { return RunCheckIn(*this); }
	if (OperationName == LoreOpNames::MarkForAdd)    { return RunMarkForAdd(*this); }
	if (OperationName == LoreOpNames::Delete)        { return RunDelete(*this); }
	if (OperationName == LoreOpNames::Revert)        { return RunRevert(*this); }
	if (OperationName == LoreOpNames::Sync)          { return SyncRevision.IsEmpty() ? RunSync(*this) : RunSyncToRevision(*this); }

	AddError(*this, FString::Printf(TEXT("Operation %s is not supported by the Lore provider."), *OperationName.ToString()));
	return false;
}
