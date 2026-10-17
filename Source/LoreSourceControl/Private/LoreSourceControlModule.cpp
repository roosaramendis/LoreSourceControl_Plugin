#include "LoreSourceControlModule.h"

#include "Features/IModularFeatures.h"
#include "Modules/ModuleManager.h"

void FLoreSourceControlModule::StartupModule()
{
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &LoreProvider);
	UE_LOG(LogLoreSC, Log, TEXT("Lore revision control provider registered."));
}

void FLoreSourceControlModule::ShutdownModule()
{
	LoreProvider.Close();
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &LoreProvider);
}

FLoreSourceControlModule& FLoreSourceControlModule::Get()
{
	return FModuleManager::GetModuleChecked<FLoreSourceControlModule>("LoreSourceControl");
}

bool FLoreSourceControlModule::IsLoaded()
{
	return FModuleManager::Get().IsModuleLoaded("LoreSourceControl");
}

IMPLEMENT_MODULE(FLoreSourceControlModule, LoreSourceControl)
