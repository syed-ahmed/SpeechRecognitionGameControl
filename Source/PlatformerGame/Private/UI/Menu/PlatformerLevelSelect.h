// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "GameMenuBuilder.h"

class FPlatformerLevelSelect : public FGameMenuPage
{
public:
	/** sets owning player controller */
	void MakeMenu(TWeakObjectPtr<APlayerController> _PCOwner);

	void OnUIPlayStreets();
	void GoBack();
	void ShowLoadingScreen();
	void OnMenuHidden();
};