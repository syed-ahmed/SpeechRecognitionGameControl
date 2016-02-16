## Synopsis
This is a simple program in C that uses the Pocketsphinx library by CMU. The library is used along with Unreal Engine 4 to demo a voice activated control to complete an action in a game.

## Code Example
All the coding is done in the SpeechRecognitionGameControl/Source/PlatformerGame/Private/Player/PlatformerCharacter.cpp
Since the game files are huge (1.6GB) I am just uploading the src. If you look into the PlatformerCharacter.cpp code,
the speech recognition code is in a extern C {} block. The code listens to the mic continously for the words "jump" and "slide". The OnStartJump, OnStartSlide, OnEndJump and OnEndSlide are triggered accordingly with the words.

## Unit Testing
I did not automate the unit testing since I don't have much background in testing.
However, I did a manual unit testing by following the steps:
- put breakpoints around the OnStartJump code block in XCode
- Run the game in debug mode
- Speak "Jump" into the microphone
- Test passes if code executes through breakpoints
- put breakpoints on OnStartSlide, OnEndJump and OnEndSlide and repeat the procedure
