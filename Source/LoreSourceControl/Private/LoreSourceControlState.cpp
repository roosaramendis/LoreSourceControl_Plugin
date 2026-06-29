#include "LoreSourceControlState.h"

#include "Styling/AppStyle.h"

FText FLoreSourceControlState::GetDisplayName() const
{
    if (bIsAdded)    return NSLOCTEXT("LoreSC", "Added",    "Add");
    if (bIsModified) return NSLOCTEXT("LoreSC", "Modified", "Edit");
    if (bIsDeleted)  return NSLOCTEXT("LoreSC", "Deleted",  "Delete");
    if (bIsUnknown)  return NSLOCTEXT("LoreSC", "Unknown",  "Unknown");
    return NSLOCTEXT("LoreSC", "Synced", "Synced");
}

FText FLoreSourceControlState::GetDisplayTooltip() const
{
    if (bIsAdded)    return NSLOCTEXT("LoreSC", "AddedTip",    "Staged for addition");
    if (bIsModified) return NSLOCTEXT("LoreSC", "ModifiedTip", "Checked out for edit");
    if (bIsDeleted)  return NSLOCTEXT("LoreSC", "DeletedTip",  "Staged for deletion");
    if (bIsUnknown)  return NSLOCTEXT("LoreSC", "UnknownTip",  "Not under source control");
    return NSLOCTEXT("LoreSC", "SyncedTip", "In sync with Lore");
}

FSlateIcon FLoreSourceControlState::GetIcon() const
{
    if (bIsAdded)
        return FSlateIcon(FAppStyle::GetAppStyleSetName(),
            "SourceControl.Add");

    if (bIsModified)
        return FSlateIcon(FAppStyle::GetAppStyleSetName(),
            "SourceControl.Edit");

    if (bIsDeleted)
        return FSlateIcon(FAppStyle::GetAppStyleSetName(),
            "SourceControl.Delete");

    if (bIsUnknown)
        return FSlateIcon(FAppStyle::GetAppStyleSetName(),
            "SourceControl.Unknown");

    return FSlateIcon(FAppStyle::GetAppStyleSetName(),
        "SourceControl.CheckedIn");
}