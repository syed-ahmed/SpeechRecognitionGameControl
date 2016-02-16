// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "PlatformerGame.h"
#include "PlatformerCharacter.h"
#include "Player/PlatformerPlayerMovementComp.h"
#include "PlatformerGameMode.h"
#include "PlatformerClimbMarker.h"
#include "PlatformerPlayerController.h"

APlatformerCharacter::APlatformerCharacter(const FObjectInitializer& ObjectInitializer) 
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UPlatformerPlayerMovementComp>(ACharacter::CharacterMovementComponentName))
{
	MinSpeedForHittingWall = 200.0f;
	GetMesh()->MeshComponentUpdateFlag = EMeshComponentUpdateFlag::AlwaysTickPoseAndRefreshBones;
}

void APlatformerCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// setting initial rotation
	SetActorRotation(FRotator(0.0f, 0.0f, 0.0f));
}

void APlatformerCharacter::SetupPlayerInputComponent(UInputComponent* InputComponent)
{
	InputComponent->BindAction("Jump", IE_Pressed, this, &APlatformerCharacter::OnStartJump);
	InputComponent->BindAction("Jump", IE_Released, this, &APlatformerCharacter::OnStopJump);
	InputComponent->BindAction("Slide", IE_Pressed, this, &APlatformerCharacter::OnStartSlide);
	InputComponent->BindAction("Slide", IE_Released, this, &APlatformerCharacter::OnStopSlide);
}

bool APlatformerCharacter::IsSliding() const
{
	UPlatformerPlayerMovementComp* MoveComp = Cast<UPlatformerPlayerMovementComp>(GetCharacterMovement());
	return MoveComp && MoveComp->IsSliding();
}

void APlatformerCharacter::CheckJumpInput(float DeltaTime)
{
	if (bPressedJump)
	{
		UPlatformerPlayerMovementComp* MoveComp = Cast<UPlatformerPlayerMovementComp>(GetCharacterMovement());
		if (MoveComp && MoveComp->IsSliding())
		{
			MoveComp->TryToEndSlide();
			return;
		}
	}

	Super::CheckJumpInput(DeltaTime);
}

void APlatformerCharacter::Tick(float DeltaSeconds)
{
	// decrease anim position adjustment
	if (!AnimPositionAdjustment.IsNearlyZero())
	{
		AnimPositionAdjustment = FMath::VInterpConstantTo(AnimPositionAdjustment, FVector::ZeroVector, DeltaSeconds, 400.0f);
		GetMesh()->SetRelativeLocation(GetBaseTranslationOffset() + AnimPositionAdjustment);
	}

	if (ClimbToMarker)
	{
		// correction in case climb marker is moving
		const FVector AdjustDelta = ClimbToMarker->GetComponentLocation() - ClimbToMarkerLocation;
		if (!AdjustDelta.IsZero())
		{
			SetActorLocation(GetActorLocation() + AdjustDelta, false);
			ClimbToMarkerLocation += AdjustDelta;
		}
	}

	Super::Tick(DeltaSeconds);

}

void APlatformerCharacter::PlayRoundFinished()
{
	APlatformerGameMode* MyGame = GetWorld()->GetAuthGameMode<APlatformerGameMode>();
	const bool bWon = MyGame && MyGame->IsRoundWon();
	
	PlayAnimMontage(bWon ? WonMontage : LostMontage);

	GetCharacterMovement()->StopMovementImmediately();
	GetCharacterMovement()->DisableMovement();
}

void APlatformerCharacter::OnRoundFinished()
{
	// don't stop in mid air, will be continued from Landed() notify
	if (GetCharacterMovement()->MovementMode != MOVE_Falling)
	{
		PlayRoundFinished();
	}
}

void APlatformerCharacter::OnRoundReset()
{
	// reset animations
	if (GetMesh() && GetMesh()->AnimScriptInstance)
	{
		GetMesh()->AnimScriptInstance->Montage_Stop(0.0f);
	}

	// reset movement properties
	GetCharacterMovement()->StopMovementImmediately();
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	bPressedJump = false;
	bPressedSlide = false;
}

void APlatformerCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	APlatformerGameMode* MyGame = GetWorld()->GetAuthGameMode<APlatformerGameMode>();
	if (MyGame && MyGame->GetGameState() == EGameState::Finished)
	{
		PlayRoundFinished();
	}
}

void APlatformerCharacter::MoveBlockedBy(const FHitResult& Impact)
{
	const float ForwardDot = FVector::DotProduct(Impact.Normal, FVector::ForwardVector);
	if (GetCharacterMovement()->MovementMode != MOVE_None)
	{
		UE_LOG(LogPlatformer, Log, TEXT("Collision with %s, normal=(%f,%f,%f), dot=%f, %s"),
			*GetNameSafe(Impact.Actor.Get()),
			Impact.Normal.X, Impact.Normal.Y, Impact.Normal.Z,
			ForwardDot,
			*GetCharacterMovement()->GetMovementName());
	}

	if (GetCharacterMovement()->MovementMode == MOVE_Walking && ForwardDot < -0.9f)
	{
		UPlatformerPlayerMovementComp* MyMovement = Cast<UPlatformerPlayerMovementComp>(GetCharacterMovement());
		const float Speed = FMath::Abs(FVector::DotProduct(MyMovement->Velocity, FVector::ForwardVector));
		// if running or sliding: play bump reaction and jump over obstacle

		float Duration = 0.01f;
		if (Speed > MinSpeedForHittingWall)
		{
			Duration = PlayAnimMontage(HitWallMontage);
		}
		GetWorldTimerManager().SetTimer(TimerHandle_ClimbOverObstacle, this, &APlatformerCharacter::ClimbOverObstacle, Duration, false);
		MyMovement->PauseMovementForObstacleHit();
	}
	else if (GetCharacterMovement()->MovementMode == MOVE_Falling)
	{
		// if in mid air: try climbing to hit marker
		APlatformerClimbMarker* Marker = Cast<APlatformerClimbMarker>(Impact.Actor.Get());
		if (Marker)
		{
			ClimbToLedge(Marker);

			UPlatformerPlayerMovementComp* MyMovement = Cast<UPlatformerPlayerMovementComp>(GetCharacterMovement());
			MyMovement->PauseMovementForLedgeGrab();
		}
	}
}

void APlatformerCharacter::ResumeMovement()
{
	SetActorEnableCollision(true);

	// restore movement state and saved speed
	UPlatformerPlayerMovementComp* MyMovement = Cast<UPlatformerPlayerMovementComp>(GetCharacterMovement());
	MyMovement->RestoreMovement();

	ClimbToMarker = NULL;
}

void APlatformerCharacter::ClimbOverObstacle()
{
	// climbing over obstacle:
	// - there are three animations matching with three types of predefined obstacle heights
	// - pawn is moved using root motion, ending up on top of obstacle as animation ends

	const FVector ForwardDir = GetActorForwardVector();
	const FVector TraceStart = GetActorLocation() + ForwardDir * 150.0f + FVector(0,0,1) * (GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 150.0f);
	const FVector TraceEnd = TraceStart + FVector(0,0,-1) * 500.0f;

	FCollisionQueryParams TraceParams(true);
	FHitResult Hit;
	GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Pawn, TraceParams);

	if (Hit.bBlockingHit)
	{
		const FVector DestPosition = Hit.ImpactPoint + FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		const float ZDiff = DestPosition.Z - GetActorLocation().Z;
		UE_LOG(LogPlatformer, Log, TEXT("Climb over obstacle, Z difference: %f (%s)"), ZDiff,
			 (ZDiff < ClimbOverMidHeight) ? TEXT("small") : (ZDiff < ClimbOverBigHeight) ? TEXT("mid") : TEXT("big"));

		UAnimMontage* Montage = (ZDiff < ClimbOverMidHeight) ? ClimbOverSmallMontage : (ZDiff < ClimbOverBigHeight) ? ClimbOverMidMontage : ClimbOverBigMontage;
		
		// set flying mode since it needs Z changes. If Walking or Falling, we won't be able to apply Z changes
		// this gets reset in the ResumeMovement
		GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		SetActorEnableCollision(false);
		const float Duration = PlayAnimMontage(Montage);
		GetWorldTimerManager().SetTimer(TimerHandle_ResumeMovement, this, &APlatformerCharacter::ResumeMovement, Duration - 0.1f, false);
	}
	else
	{
		// shouldn't happen
		ResumeMovement();
	}
}

void APlatformerCharacter::ClimbToLedge(const APlatformerClimbMarker* MoveToMarker)
{
	ClimbToMarker = MoveToMarker ? MoveToMarker->FindComponentByClass<UStaticMeshComponent>() : NULL;
	ClimbToMarkerLocation = ClimbToMarker ? ClimbToMarker->GetComponentLocation() : FVector::ZeroVector;

	// place on top left corner of marker, but preserve current Y coordinate
	const FBox MarkerBox = MoveToMarker->GetMesh()->Bounds.GetBox();
	const FVector DesiredPosition(MarkerBox.Min.X, GetActorLocation().Y, MarkerBox.Max.Z);

	// climbing to ledge:
	// - pawn is placed on top of ledge (using ClimbLedgeGrabOffsetX to offset from grab point) immediately
	// - AnimPositionAdjustment modifies mesh relative location to smooth transition
	//   (mesh starts roughly at the same position, additional offset quickly decreases to zero in Tick)

	const FVector StartPosition = GetActorLocation();
	FVector AdjustedPosition = DesiredPosition;
	AdjustedPosition.X += (ClimbLedgeGrabOffsetX * GetMesh()->RelativeScale3D.X) - GetBaseTranslationOffset().X;
	AdjustedPosition.Z += GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	TeleportTo(AdjustedPosition, GetActorRotation(), false, true);

	AnimPositionAdjustment = StartPosition - (GetActorLocation() - (ClimbLedgeRootOffset * GetMesh()->RelativeScale3D));
	GetMesh()->SetRelativeLocation(GetBaseTranslationOffset() + AnimPositionAdjustment);

	const float Duration = PlayAnimMontage(ClimbLedgeMontage);
	GetWorldTimerManager().SetTimer(TimerHandle_ResumeMovement, this, &APlatformerCharacter::ResumeMovement, Duration - 0.1f, false);
}



void APlatformerCharacter::OnStopJump()
{
	bPressedJump = false;
}

void APlatformerCharacter::OnStartSlide()
{
	APlatformerGameMode* MyGame = GetWorld()->GetAuthGameMode<APlatformerGameMode>();
	APlatformerPlayerController* MyPC = Cast<APlatformerPlayerController>(Controller);
	if (MyPC)
	{
		if (MyPC->TryStartingGame())
		{
			return;
		}
		
		if (!MyPC->IsMoveInputIgnored() &&
			MyGame && MyGame->IsRoundInProgress())
		{
			bPressedSlide = true;
		}
	}
}

void APlatformerCharacter::OnStopSlide()
{
	bPressedSlide = false;
}

void APlatformerCharacter::PlaySlideStarted()
{
	if (SlideSound)
	{
		SlideAC = UGameplayStatics::SpawnSoundAttached(SlideSound, GetMesh());
	}
}

void APlatformerCharacter::PlaySlideFinished()
{
	if (SlideAC)
	{
		SlideAC->Stop();
		SlideAC = NULL;
	}
}

bool APlatformerCharacter::WantsToSlide() const
{
	return bPressedSlide;
}

float APlatformerCharacter::GetCameraHeightChangeThreshold() const
{
	return CameraHeightChangeThreshold;
}

extern "C" {
    /* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
    /* ====================================================================
     * Copyright (c) 1999-2010 Carnegie Mellon University.  All rights
     * reserved.
     *
     * Redistribution and use in source and binary forms, with or without
     * modification, are permitted provided that the following conditions
     * are met:
     *
     * 1. Redistributions of source code must retain the above copyright
     *    notice, this list of conditions and the following disclaimer.
     *
     * 2. Redistributions in binary form must reproduce the above copyright
     *    notice, this list of conditions and the following disclaimer in
     *    the documentation and/or other materials provided with the
     *    distribution.
     *
     * This work was supported in part by funding from the Defense Advanced
     * Research Projects Agency and the National Science Foundation of the
     * United States of America, and the CMU Sphinx Speech Consortium.
     *
     * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND
     * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
     * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
     * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
     * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
     * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
     * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
     * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
     * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
     * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
     * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
     *
     * ====================================================================
     *
     */
    
    /*
     * continuous.c - Simple pocketsphinx command-line application to test
     *                both continuous listening/silence filtering from microphone
     *                and continuous file transcription.
     */
    
    /*
     * This is a simple example of pocketsphinx application that uses continuous listening
     * with silence filtering to automatically segment a continuous stream of audio input
     * into utterances that are then decoded.
     *
     * Remarks:
     *   - Each utterance is ended when a silence segment of at least 1 sec is recognized.
     *   - Single-threaded implementation for portability.
     *   - Uses audio library; can be replaced with an equivalent custom library.
     */
    
#include "stdio.h"
#include "string.h"
#include "assert.h"
    
#if defined(_WIN32) && !defined(__CYGWIN__)
#include "windows.h"
#else
#include "sys/select.h"
#endif
    
#include "/usr/local/Cellar/cmu-sphinxbase/HEAD/include/sphinxbase/err.h"
#include "/usr/local/Cellar/cmu-sphinxbase/HEAD/include/sphinxbase/ad.h"
#include "/usr/local/Cellar/cmu-sphinxbase/HEAD/include/sphinxbase/sphinxbase_export.h"
#include "/usr/local/Cellar/cmu-pocketsphinx/HEAD/include/pocketsphinx/pocketsphinx.h"
    
    static const arg_t cont_args_def[] = {
        POCKETSPHINX_OPTIONS,
        /* Argument file. */
        {"-argfile",
            ARG_STRING,
            NULL,
            "Argument file giving extra arguments."},
        {"-adcdev",
            ARG_STRING,
            NULL,
            "Name of audio device to use for input."},
        {"-infile",
            ARG_STRING,
            NULL,
            "Audio file to transcribe."},
        {"-inmic",
            ARG_BOOLEAN,
            "no",
            "Transcribe audio from microphone."},
        {"-time",
            ARG_BOOLEAN,
            "no",
            "Print word times in file transcription."},
        CMDLN_EMPTY_OPTION
    };
    
    static ps_decoder_t *ps;
    static cmd_ln_t *config;
    static FILE *rawfd;
    
    /* Sleep for specified msec */
    static void
    sleep_msec(int32 ms)
    {
#if (defined(_WIN32) && !defined(GNUWINCE)) || defined(_WIN32_WCE)
        Sleep(ms);
#else
        /* ------------------- Unix ------------------ */
        struct timeval tmo;
        
        tmo.tv_sec = 0;
        tmo.tv_usec = ms * 1000;
        
        select(0, NULL, NULL, NULL, &tmo);
#endif
    }
    
    /*
     * Main utterance processing loop:
     *     for (;;) {
     *        start utterance and wait for speech to process
     *        decoding till end-of-utterance silence will be detected
     *        print utterance result;
     *     }
     */
    static void
    recognize_from_microphone()
    {
        ad_rec_t *ad;
        int16 adbuf[2048];
        uint8 utt_started, in_speech;
        int32 k;
        char const *hyp;
        
        if ((ad = ad_open_dev(cmd_ln_str_r(config, "-adcdev"),
                              (int) cmd_ln_float32_r(config,
                                                     "-samprate"))) == NULL)
            E_FATAL("Failed to open audio device\n");
        if (ad_start_rec(ad) < 0)
            E_FATAL("Failed to start recording\n");
        
        if (ps_start_utt(ps) < 0)
            E_FATAL("Failed to start utterance\n");
        utt_started = FALSE;
        E_INFO("Ready....\n");
        
        for (;;) {
            if ((k = ad_read(ad, adbuf, 2048)) < 0)
                E_FATAL("Failed to read audio\n");
            ps_process_raw(ps, adbuf, k, FALSE, FALSE);
            in_speech = ps_get_in_speech(ps);
            if (in_speech && !utt_started) {
                utt_started = TRUE;
                E_INFO("Listening...\n");
            }
            if (!in_speech && utt_started) {
                /* speech -> silence transition, time to start new utterance  */
                ps_end_utt(ps);
                hyp = ps_get_hyp(ps, NULL );
                if (hyp != NULL) {
                    printf("The Words are: ");
                    printf("%s\n", hyp);
                    fflush(stdout);
                }
                
                if (ps_start_utt(ps) < 0)
                    E_FATAL("Failed to start utterance\n");
                utt_started = FALSE;
                E_INFO("Ready....\n");
            }
            sleep_msec(100);
        }
        ad_close(ad);
    }
    
}

void APlatformerCharacter::OnStartJump()
{
    recognize_from_microphone();
    APlatformerGameMode* const MyGame = GetWorld()->GetAuthGameMode<APlatformerGameMode>();
    APlatformerPlayerController* MyPC = Cast<APlatformerPlayerController>(Controller);
    if (MyPC)
    {
        if (MyPC->TryStartingGame())
        {
            return;
        }
        
        if (!MyPC->IsMoveInputIgnored() &&
            MyGame && MyGame->IsRoundInProgress())
        {
            bPressedJump = true;
        }
    }
}
