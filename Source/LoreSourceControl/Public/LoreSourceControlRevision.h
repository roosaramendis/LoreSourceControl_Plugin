#pragma once

// One entry in the File History window (one revision of one file).
// Get() extracts that version of the file so Unreal can open or diff it.

#include "CoreMinimal.h"
#include "ISourceControlRevision.h"
#include "LoreUtils.h"

class FLoreSourceControlRevision : public ISourceControlRevision
{
public:
	FLoreSourceControlRevision(const FString& InFilename, const FLoreHistoryEntry& InEntry, const FLoreContext& InContext);

	// ---- ISourceControlRevision ----
	virtual bool Get(FString& InOutFilename, EConcurrency::Type InConcurrency = EConcurrency::Synchronous) const override;
	virtual bool GetAnnotated(TArray<FAnnotationLine>& OutLines) const override { return false; }
	virtual bool GetAnnotated(FString& InOutFilename) const override { return false; }
	virtual const FString& GetFilename() const override { return Filename; }
	virtual int32 GetRevisionNumber() const override { return Entry.Number; }
	virtual const FString& GetRevision() const override { return RevisionText; }
	virtual const FString& GetDescription() const override { return Entry.Message; }
	virtual const FString& GetUserName() const override { return UserName; }
	virtual const FString& GetClientSpec() const override { return ClientSpec; }
	virtual const FString& GetAction() const override { return Action; }
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetBranchSource() const override { return nullptr; }
	virtual const FDateTime& GetDate() const override { return Entry.Date; }
	virtual int32 GetCheckInIdentifier() const override { return Entry.Number; }
	virtual int32 GetFileSize() const override { return 0; }

	const FString& GetSignature() const { return Entry.Signature; }

private:
	FString Filename;      // absolute path of the file in the project
	FLoreHistoryEntry Entry;
	FLoreContext Ctx;      // how to run lore (binary, repo, identity)
	FString RevisionText;  // "5"
	FString UserName;
	FString ClientSpec;    // unused by Lore, Unreal wants a reference
	FString Action;        // "add" / "edit" / "delete" (Perforce words, Unreal uses them for icons)
};
