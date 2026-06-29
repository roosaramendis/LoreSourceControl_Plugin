#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "Features/IModularFeatures.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FLoreSourceControlModule"

void FLoreSourceControlModule::StartupModule()
{
    IModularFeatures::Get().RegisterModularFeature(
        "SourceControl",
        &LoreProvider
    );
    UE_LOG(LogTemp, Log, TEXT("LoreSourceControl: Provider registered!"));
}

void FLoreSourceControlModule::ShutdownModule()
{
    LoreProvider.Close();
    IModularFeatures::Get().UnregisterModularFeature(
        "SourceControl",
        &LoreProvider
    );
}

FLoreSourceControlProvider& FLoreSourceControlModule::GetProvider()
{
    return LoreProvider;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLoreSourceControlModule, LoreSourceControl)