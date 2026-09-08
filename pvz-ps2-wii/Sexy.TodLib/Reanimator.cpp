#include "TodDebug.h"
#include "TodCommon.h"
#include "Definition.h"
#include "Reanimator.h"
#include "../LawnApp.h"
#include "Attachment.h"
#include "ReanimAtlas.h"
#include "EffectSystem.h"
#include "../GameConstants.h"
#include "../Resources.h"
#include "graphics/Font.h"
#include "misc/PerfTimer.h"
#include "graphics/MemoryImage.h"
#include "misc/BigBlockArena.h"
#ifdef WII_PLATFORM
#include "misc/GraphicsBlockArena.h"
#endif
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
#include <cstdlib>
#include <cstring>
#endif
#include <set>
#include <vector>

#ifdef PS2_PLATFORM
#include <pthread.h>
#include "graphics/GLInterface.h"
#include "platform/ps2/Ps2PvzServices.h"
#endif

#ifdef PS2_PLATFORM
// Park the audio mixer around reanim file I/O. On PS2 the mixer thread issues
// audsrv SIF RPCs continuously; a reanim's compiled file plus its PNGs are read
// on the main thread — and with LOW_MEMORY there is no preload, so this fires
// the first time each plant/zombie/effect appears mid-game (Plant::Startup,
// board effects, save load). An overlapping fio wedges the shared IOP on real
// hardware. Forward-declared to avoid pulling the PS2 sound header in here.
namespace Sexy { void Ps2MixerIoPauseBegin(); void Ps2MixerIoPauseEnd(); }
namespace {
	struct Ps2ReanimIoPause {
		Ps2ReanimIoPause()  { Sexy::Ps2MixerIoPauseBegin(); }
		~Ps2ReanimIoPause() { Sexy::Ps2MixerIoPauseEnd(); }
	};
}
#define PS2_PAUSE_MIXER_FOR_IO() Ps2ReanimIoPause aPs2ReanimIoPause_
#else
#define PS2_PAUSE_MIXER_FOR_IO() ((void)0)
#endif

unsigned int gReanimatorDefCount;                     //[0x6A9EE4]
ReanimatorDefinition* gReanimatorDefArray;   //[0x6A9EE8]
unsigned int gReanimationParamArraySize;              //[0x6A9EEC]
ReanimationParams* gReanimationParamArray;   //[0x6A9EF0]

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
struct ReanimatorAttacherCache
{
	const char* mSourceText;
	const char* mTrackName;
	float mAnimRate;
	ReanimationType mReanimationType;
	unsigned short mTrackNameLength;
	unsigned char mLoopType;
	unsigned char mValid;
};

static const unsigned int REANIMATOR_ATTACHER_CACHE_SIZE = 256U;
static ReanimatorAttacherCache gReanimatorAttacherCache[REANIMATOR_ATTACHER_CACHE_SIZE];
static ReanimatorAttacherCache& ReanimatorGetAttacherCached(const ReanimatorTransform& theTransform);

struct ReanimatorBasePoseCacheEntry
{
	const ReanimatorDefinition* mDefinition;
	int mTrackIndex;
	int mBasePoseFrame;
	SexyTransform2D mInverseMatrix;
	bool mValid;
};

static const unsigned int REANIMATOR_BASE_POSE_CACHE_SIZE = 256U;
static ReanimatorBasePoseCacheEntry gReanimatorBasePoseCache[REANIMATOR_BASE_POSE_CACHE_SIZE];

static void ReanimatorInvalidateBasePoseCache(const ReanimatorDefinition* theDefinition)
{
	for (unsigned int i = 0; i < REANIMATOR_BASE_POSE_CACHE_SIZE; i++)
	{
		ReanimatorBasePoseCacheEntry& aEntry = gReanimatorBasePoseCache[i];
		if (aEntry.mValid && (theDefinition == nullptr || aEntry.mDefinition == theDefinition))
			aEntry.mValid = false;
	}
}

static void ReanimatorGetBasePoseInverseCached(Reanimation* theReanimation, int theTrackIndex, SexyTransform2D& theInverseMatrix)
{
	if (theReanimation->mFrameBasePose == NO_BASE_POSE)
	{
		theInverseMatrix.LoadIdentity();
		return;
	}

	int aBasePoseFrame = theReanimation->mFrameBasePose == -1 ? theReanimation->mFrameStart : theReanimation->mFrameBasePose;
	uintptr_t aHash = ((uintptr_t)theReanimation->mDefinition >> 4) ^ (unsigned int)(theTrackIndex * 131U) ^ (unsigned int)(aBasePoseFrame * 17U);
	ReanimatorBasePoseCacheEntry& aEntry = gReanimatorBasePoseCache[aHash & (REANIMATOR_BASE_POSE_CACHE_SIZE - 1U)];
	if (aEntry.mValid && aEntry.mDefinition == theReanimation->mDefinition &&
		aEntry.mTrackIndex == theTrackIndex && aEntry.mBasePoseFrame == aBasePoseFrame)
	{
		theInverseMatrix = aEntry.mInverseMatrix;
		return;
	}

	SexyTransform2D aBasePoseMatrix;
	theReanimation->GetTrackBasePoseMatrix(theTrackIndex, aBasePoseMatrix);
	SexyMatrix3Inverse(aBasePoseMatrix, aEntry.mInverseMatrix);
	aEntry.mDefinition = theReanimation->mDefinition;
	aEntry.mTrackIndex = theTrackIndex;
	aEntry.mBasePoseFrame = aBasePoseFrame;
	aEntry.mValid = true;
	theInverseMatrix = aEntry.mInverseMatrix;
}
#endif

ReanimationParams gLawnReanimationArray[(int)ReanimationType::NUM_REANIMS] = { //0x6A1340
	{ ReanimationType::REANIM_LOADBAR_SPROUT,                       "reanim/LoadBar_sprout.reanim",                    1 },
	{ ReanimationType::REANIM_LOADBAR_ZOMBIEHEAD,                   "reanim/LoadBar_Zombiehead.reanim",                1 },
	{ ReanimationType::REANIM_SODROLL,                              "reanim/SodRoll.reanim",                           0 },
	{ ReanimationType::REANIM_FINAL_WAVE,                           "reanim/FinalWave.reanim",                         1 },
	{ ReanimationType::REANIM_PEASHOOTER,                           "reanim/PeaShooterSingle.reanim",                  0 },
	{ ReanimationType::REANIM_WALLNUT,                              "reanim/Wallnut.reanim",                           0 },
	{ ReanimationType::REANIM_LILYPAD,                              "reanim/Lilypad.reanim",                           0 },
	{ ReanimationType::REANIM_SUNFLOWER,                            "reanim/SunFlower.reanim",                         0 },
	{ ReanimationType::REANIM_LAWNMOWER,                            "reanim/LawnMower.reanim",                         0 },
	{ ReanimationType::REANIM_READYSETPLANT,                        "reanim/StartReadySetPlant.reanim",                1 },
	{ ReanimationType::REANIM_CHERRYBOMB,                           "reanim/CherryBomb.reanim",                        0 },
	{ ReanimationType::REANIM_SQUASH,                               "reanim/Squash.reanim",                            0 },
	{ ReanimationType::REANIM_DOOMSHROOM,                           "reanim/DoomShroom.reanim",                        0 },
	{ ReanimationType::REANIM_SNOWPEA,                              "reanim/SnowPea.reanim",                           0 },
	{ ReanimationType::REANIM_REPEATER,                             "reanim/PeaShooter.reanim",                        0 },
	{ ReanimationType::REANIM_SUNSHROOM,                            "reanim/SunShroom.reanim",                         0 },
	{ ReanimationType::REANIM_TALLNUT,                              "reanim/Tallnut.reanim",                           0 },
	{ ReanimationType::REANIM_FUMESHROOM,                           "reanim/Fumeshroom.reanim",                        0 },
	{ ReanimationType::REANIM_PUFFSHROOM,                           "reanim/Puffshroom.reanim",                        0 },
	{ ReanimationType::REANIM_HYPNOSHROOM,                          "reanim/Hypnoshroom.reanim",                       0 },
	{ ReanimationType::REANIM_CHOMPER,                              "reanim/Chomper.reanim",                           0 },
	{ ReanimationType::REANIM_ZOMBIE,                               "reanim/Zombie.reanim",                            0 },
	{ ReanimationType::REANIM_SUN,                                  "reanim/Sun.reanim",                               0 },
	{ ReanimationType::REANIM_POTATOMINE,                           "reanim/PotatoMine.reanim",                        0 },
	{ ReanimationType::REANIM_SPIKEWEED,                            "reanim/Caltrop.reanim",                           0 },
	{ ReanimationType::REANIM_SPIKEROCK,                            "reanim/SpikeRock.reanim",                         0 },
	{ ReanimationType::REANIM_THREEPEATER,                          "reanim/ThreePeater.reanim",                       0 },
	{ ReanimationType::REANIM_MARIGOLD,                             "reanim/Marigold.reanim",                          0 },
	{ ReanimationType::REANIM_ICESHROOM,                            "reanim/IceShroom.reanim",                         0 },
	{ ReanimationType::REANIM_ZOMBIE_FOOTBALL,                      "reanim/Zombie_football.reanim",                   0 },
	{ ReanimationType::REANIM_ZOMBIE_NEWSPAPER,                     "reanim/Zombie_paper.reanim",                      0 },
	{ ReanimationType::REANIM_ZOMBIE_ZAMBONI,                       "reanim/Zombie_zamboni.reanim",                    0 },
	{ ReanimationType::REANIM_SPLASH,                               "reanim/splash.reanim",                            0 },
	{ ReanimationType::REANIM_JALAPENO,                             "reanim/Jalapeno.reanim",                          0 },
	{ ReanimationType::REANIM_JALAPENO_FIRE,                        "reanim/fire.reanim",                              0 },
	{ ReanimationType::REANIM_COIN_SILVER,                          "reanim/Coin_silver.reanim",                       0 },
	{ ReanimationType::REANIM_ZOMBIE_CHARRED,                       "reanim/Zombie_charred.reanim",                    0 },
	{ ReanimationType::REANIM_ZOMBIE_CHARRED_IMP,                   "reanim/Zombie_charred_imp.reanim",                0 },
	{ ReanimationType::REANIM_ZOMBIE_CHARRED_DIGGER,                "reanim/Zombie_charred_digger.reanim",             0 },
	{ ReanimationType::REANIM_ZOMBIE_CHARRED_ZAMBONI,               "reanim/Zombie_charred_zamboni.reanim",            0 },
	{ ReanimationType::REANIM_ZOMBIE_CHARRED_CATAPULT,              "reanim/Zombie_charred_catapult.reanim",           0 },
	{ ReanimationType::REANIM_ZOMBIE_CHARRED_GARGANTUAR,            "reanim/Zombie_charred_gargantuar.reanim",         0 },
	{ ReanimationType::REANIM_SCRAREYSHROOM,                        "reanim/ScaredyShroom.reanim",                     0 },
	{ ReanimationType::REANIM_PUMPKIN,                              "reanim/Pumpkin.reanim",                           0 },
	{ ReanimationType::REANIM_PLANTERN,                             "reanim/Plantern.reanim",                          0 },
	{ ReanimationType::REANIM_TORCHWOOD,                            "reanim/Torchwood.reanim",                         0 },
	{ ReanimationType::REANIM_SPLITPEA,                             "reanim/SplitPea.reanim",                          0 },
	{ ReanimationType::REANIM_SEASHROOM,                            "reanim/SeaShroom.reanim",                         0 },
	{ ReanimationType::REANIM_BLOVER,                               "reanim/Blover.reanim",                            0 },
	{ ReanimationType::REANIM_FLOWER_POT,                           "reanim/Pot.reanim",                               0 },
	{ ReanimationType::REANIM_CACTUS,                               "reanim/Cactus.reanim",                            0 },
	{ ReanimationType::REANIM_DANCER,                               "reanim/Zombie_disco.reanim",						0 }, // @Patoke: GOTY has different reanim name
	{ ReanimationType::REANIM_TANGLEKELP,                           "reanim/Tanglekelp.reanim",                        0 },
	{ ReanimationType::REANIM_STARFRUIT,                            "reanim/Starfruit.reanim",                         0 },
	{ ReanimationType::REANIM_POLEVAULTER,                          "reanim/Zombie_polevaulter.reanim",                0 },
	{ ReanimationType::REANIM_BALLOON,                              "reanim/Zombie_balloon.reanim",                    0 },
	{ ReanimationType::REANIM_GARGANTUAR,                           "reanim/Zombie_gargantuar.reanim",                 0 },
	{ ReanimationType::REANIM_IMP,                                  "reanim/Zombie_imp.reanim",                        0 },
	{ ReanimationType::REANIM_DIGGER,                               "reanim/Zombie_digger.reanim",                     0 },
	{ ReanimationType::REANIM_DIGGER_DIRT,                          "reanim/Digger_rising_dirt.reanim",                0 },
	{ ReanimationType::REANIM_ZOMBIE_DOLPHINRIDER,                  "reanim/Zombie_dolphinrider.reanim",               0 },
	{ ReanimationType::REANIM_POGO,                                 "reanim/Zombie_pogo.reanim",                       0 },
	{ ReanimationType::REANIM_BACKUP_DANCER,                        "reanim/Zombie_backup.reanim",                     0 }, // @Patoke: GOTY has different reanim name
	{ ReanimationType::REANIM_BOBSLED,                              "reanim/Zombie_bobsled.reanim",                    0 },
	{ ReanimationType::REANIM_JACKINTHEBOX,                         "reanim/Zombie_jackbox.reanim",                    0 },
	{ ReanimationType::REANIM_SNORKEL,                              "reanim/Zombie_snorkle.reanim",                    0 },
	{ ReanimationType::REANIM_BUNGEE,                               "reanim/Zombie_bungi.reanim",                      0 },
	{ ReanimationType::REANIM_CATAPULT,                             "reanim/Zombie_catapult.reanim",                   0 },
	{ ReanimationType::REANIM_LADDER,                               "reanim/Zombie_ladder.reanim",                     0 },
	{ ReanimationType::REANIM_PUFF,                                 "reanim/Puff.reanim",                              0 },
	{ ReanimationType::REANIM_SLEEPING,                             "reanim/Z.reanim",                                 0 },
	{ ReanimationType::REANIM_GRAVE_BUSTER,                         "reanim/Gravebuster.reanim",                       0 },
	{ ReanimationType::REANIM_ZOMBIES_WON,                          "reanim/ZombiesWon.reanim",                        1 },
	{ ReanimationType::REANIM_MAGNETSHROOM,                         "reanim/Magnetshroom.reanim",                      0 },
	{ ReanimationType::REANIM_BOSS,                                 "reanim/Zombie_boss.reanim",                       0 },
	{ ReanimationType::REANIM_CABBAGEPULT,                          "reanim/Cabbagepult.reanim",                       0 },
	{ ReanimationType::REANIM_KERNELPULT,                           "reanim/Cornpult.reanim",                          0 },
	{ ReanimationType::REANIM_MELONPULT,                            "reanim/Melonpult.reanim",                         0 },
	{ ReanimationType::REANIM_COFFEEBEAN,                           "reanim/Coffeebean.reanim",                        1 },
	{ ReanimationType::REANIM_UMBRELLALEAF,                         "reanim/Umbrellaleaf.reanim",                      0 },
	{ ReanimationType::REANIM_GATLINGPEA,                           "reanim/GatlingPea.reanim",                        0 },
	{ ReanimationType::REANIM_CATTAIL,                              "reanim/Cattail.reanim",                           0 },
	{ ReanimationType::REANIM_GLOOMSHROOM,                          "reanim/GloomShroom.reanim",                       0 },
	{ ReanimationType::REANIM_BOSS_ICEBALL,                         "reanim/Zombie_boss_iceball.reanim",               1 },
	{ ReanimationType::REANIM_BOSS_FIREBALL,                        "reanim/Zombie_boss_fireball.reanim",              1 },
	{ ReanimationType::REANIM_COBCANNON,                            "reanim/CobCannon.reanim",                         0 },
	{ ReanimationType::REANIM_GARLIC,                               "reanim/Garlic.reanim",                            0 },
	{ ReanimationType::REANIM_GOLD_MAGNET,                          "reanim/GoldMagnet.reanim",                        0 },
	{ ReanimationType::REANIM_WINTER_MELON,                         "reanim/WinterMelon.reanim",                       0 },
	{ ReanimationType::REANIM_TWIN_SUNFLOWER,                       "reanim/TwinSunflower.reanim",                     0 },
	{ ReanimationType::REANIM_POOL_CLEANER,                         "reanim/PoolCleaner.reanim",                       0 },
	{ ReanimationType::REANIM_ROOF_CLEANER,                         "reanim/RoofCleaner.reanim",                       0 },
	{ ReanimationType::REANIM_FIRE_PEA,                             "reanim/FirePea.reanim",                           0 },
	{ ReanimationType::REANIM_IMITATER,                             "reanim/Imitater.reanim",                          0 },
	{ ReanimationType::REANIM_YETI,                                 "reanim/Zombie_yeti.reanim",                       0 },
	{ ReanimationType::REANIM_BOSS_DRIVER,                          "reanim/Zombie_Boss_driver.reanim",                0 },
	{ ReanimationType::REANIM_LAWN_MOWERED_ZOMBIE,                  "reanim/LawnMoweredZombie.reanim",                 0 },
	{ ReanimationType::REANIM_CRAZY_DAVE,                           "reanim/CrazyDave.reanim",                         1 },
	{ ReanimationType::REANIM_TEXT_FADE_ON,                         "reanim/TextFadeOn.reanim",                        0 },
	{ ReanimationType::REANIM_HAMMER,                               "reanim/Hammer.reanim",                            0 },
	{ ReanimationType::REANIM_SLOT_MACHINE_HANDLE,                  "reanim/SlotMachine.reanim",                       0 },
	{ ReanimationType::REANIM_CREDITS_FOOTBALL,                     "reanim/Credits_Football.reanim",                  1 },
	{ ReanimationType::REANIM_CREDITS_JACKBOX,                      "reanim/Credits_Jackbox.reanim",                   1 },
	{ ReanimationType::REANIM_SELECTOR_SCREEN,                      "reanim/SelectorScreen.reanim",                    3 },
	{ ReanimationType::REANIM_PORTAL_CIRCLE,                        "reanim/Portal_Circle.reanim",                     0 },
	{ ReanimationType::REANIM_PORTAL_SQUARE,                        "reanim/Portal_Square.reanim",                     0 },
	{ ReanimationType::REANIM_ZENGARDEN_SPROUT,                     "reanim/ZenGarden_sprout.reanim",                  0 },
	{ ReanimationType::REANIM_ZENGARDEN_WATERINGCAN,                "reanim/ZenGarden_wateringcan.reanim",             1 },
	{ ReanimationType::REANIM_ZENGARDEN_FERTILIZER,                 "reanim/ZenGarden_fertilizer.reanim",              1 },
	{ ReanimationType::REANIM_ZENGARDEN_BUGSPRAY,                   "reanim/ZenGarden_bugspray.reanim",                1 },
	{ ReanimationType::REANIM_ZENGARDEN_PHONOGRAPH,                 "reanim/ZenGarden_phonograph.reanim",              1 },
	{ ReanimationType::REANIM_DIAMOND,                              "reanim/Diamond.reanim",                           0 },
	{ ReanimationType::REANIM_ZOMBIE_HAND,                          "reanim/Zombie_hand.reanim",                       1 },
	{ ReanimationType::REANIM_STINKY,                               "reanim/Stinky.reanim",                            0 },
	{ ReanimationType::REANIM_RAKE,                                 "reanim/Rake.reanim",                              0 },
	{ ReanimationType::REANIM_RAIN_CIRCLE,                          "reanim/Rain_circle.reanim",                       0 },
	{ ReanimationType::REANIM_RAIN_SPLASH,                          "reanim/Rain_splash.reanim",                       0 },
	{ ReanimationType::REANIM_ZOMBIE_SURPRISE,                      "reanim/Zombie_surprise.reanim",                   0 },
	{ ReanimationType::REANIM_COIN_GOLD,                            "reanim/Coin_gold.reanim",                         0 },
	{ ReanimationType::REANIM_TREEOFWISDOM,                         "reanim/TreeOfWisdom.reanim",                      1 },
	{ ReanimationType::REANIM_TREEOFWISDOM_CLOUDS,                  "reanim/TreeOfWisdomClouds.reanim",                1 },
	{ ReanimationType::REANIM_TREEOFWISDOM_TREEFOOD,                "reanim/TreeFood.reanim",                          1 },
	{ ReanimationType::REANIM_CREDITS_MAIN,                         "reanim/Credits_Main.reanim",                      3 },
	{ ReanimationType::REANIM_CREDITS_MAIN2,                        "reanim/Credits_Main2.reanim",                     3 },
	{ ReanimationType::REANIM_CREDITS_MAIN3,                        "reanim/Credits_Main3.reanim",                     3 },
	{ ReanimationType::REANIM_ZOMBIE_CREDITS_DANCE,                 "reanim/Zombie_credits_dance.reanim",              0 },
	{ ReanimationType::REANIM_CREDITS_STAGE,                        "reanim/Credits_stage.reanim",                     1 },
	{ ReanimationType::REANIM_CREDITS_BIGBRAIN,                     "reanim/Credits_BigBrain.reanim",                  1 },
	{ ReanimationType::REANIM_CREDITS_FLOWER_PETALS,                "reanim/Credits_Flower_petals.reanim",             1 },
	{ ReanimationType::REANIM_CREDITS_INFANTRY,                     "reanim/Credits_Infantry.reanim",                  1 },
	{ ReanimationType::REANIM_CREDITS_THROAT,                       "reanim/Credits_Throat.reanim",                    1 },
	{ ReanimationType::REANIM_CREDITS_CRAZYDAVE,                    "reanim/Credits_CrazyDave.reanim",                 1 },
	{ ReanimationType::REANIM_CREDITS_BOSSDANCE,                    "reanim/Credits_Bossdance.reanim",                 1 },
	{ ReanimationType::REANIM_ZOMBIE_CREDITS_SCREEN_DOOR,           "reanim/Zombie_Credits_Screendoor.reanim",         1 },
	{ ReanimationType::REANIM_ZOMBIE_CREDITS_CONEHEAD,              "reanim/Zombie_Credits_Conehead.reanim",           1 },
	{ ReanimationType::REANIM_CREDITS_ZOMBIEARMY1,                  "reanim/Credits_ZombieArmy1.reanim",               1 },
	{ ReanimationType::REANIM_CREDITS_ZOMBIEARMY2,                  "reanim/Credits_ZombieArmy2.reanim",               1 },
	{ ReanimationType::REANIM_CREDITS_TOMBSTONES,                   "reanim/Credits_Tombstones.reanim",                1 },
	{ ReanimationType::REANIM_CREDITS_SOLARPOWER,                   "reanim/Credits_SolarPower.reanim",                1 },
	{ ReanimationType::REANIM_CREDITS_ANYHOUR,                      "reanim/Credits_Anyhour.reanim",                   3 },
	{ ReanimationType::REANIM_CREDITS_WEARETHEUNDEAD,               "reanim/Credits_WeAreTheUndead.reanim",            1 },
	{ ReanimationType::REANIM_CREDITS_DISCOLIGHTS,                  "reanim/Credits_DiscoLights.reanim",               1 },
	{ ReanimationType::REANIM_FLAG,                                 "reanim/Zombie_FlagPole.reanim",                   0 },
};

//0x471540
ReanimatorTransform::ReanimatorTransform() :
	mTransX(DEFAULT_FIELD_PLACEHOLDER),
	mTransY(DEFAULT_FIELD_PLACEHOLDER),
	mSkewX(DEFAULT_FIELD_PLACEHOLDER),
	mSkewY(DEFAULT_FIELD_PLACEHOLDER),
	mScaleX(DEFAULT_FIELD_PLACEHOLDER),
	mScaleY(DEFAULT_FIELD_PLACEHOLDER),
	mFrame(DEFAULT_FIELD_PLACEHOLDER),
	mAlpha(DEFAULT_FIELD_PLACEHOLDER),
	mImage(nullptr),
	mFont(nullptr),
	mText("") { }

inline void ReanimationFillInMissingData(float& thePrev, float& theValue)
{
	if (theValue == DEFAULT_FIELD_PLACEHOLDER)
		theValue = thePrev;  // 若当前帧上的值未设定，则以前一帧的数值赋值当前帧
	else
		thePrev = theValue;  // 否则，将当前帧的数据记录为“前一帧的数据”
}

inline void ReanimationFillInMissingData(void*& thePrev, void*& theValue)
{
	if (theValue == nullptr)
		theValue = thePrev;
	else
		thePrev = theValue;
}

//0x4715F0 : (*def, eax = string& fileName)  //esp -= 0x4
bool ReanimationLoadDefinition(const SexyString& theFileName, ReanimatorDefinition* theDefinition)
{
	if (!DefinitionLoadXML(theFileName, &gReanimatorDefMap, theDefinition))
		return false;

	for (int aTrackIndex = 0; aTrackIndex < theDefinition->mTracks.count; aTrackIndex++)
	{
		ReanimatorTrack* aTrack = &theDefinition->mTracks.tracks[aTrackIndex];
		float aPrevTransX = 0.0f;
		float aPrevTransY = 0.0f;
		float aPrevSkewX = 0.0f;
		float aPrevSkewY = 0.0f;
		float aPrevScaleX = 1.0f;
		float aPrevScaleY = 1.0f;
		float aPrevFrame = 0.0f;
		float aPrevAlpha = 1.0f;
		Image* aPrevImage = nullptr;
		_Font* aPrevFont = nullptr;
		const char* aPrevText = "";

		// 遍历每一帧，依次用前一帧的数据填充后一帧的未定义数据，并重新记录前一帧的数据
		for (int i = 0; i < aTrack->mTransforms.count; i++)
		{
			ReanimatorTransform& aTransform = aTrack->mTransforms.mTransforms[i];
			ReanimationFillInMissingData(aPrevTransX, aTransform.mTransX);
			ReanimationFillInMissingData(aPrevTransY, aTransform.mTransY);
			ReanimationFillInMissingData(aPrevSkewX, aTransform.mSkewX);
			ReanimationFillInMissingData(aPrevSkewY, aTransform.mSkewY);
			ReanimationFillInMissingData(aPrevScaleX, aTransform.mScaleX);
			ReanimationFillInMissingData(aPrevScaleY, aTransform.mScaleY);
			ReanimationFillInMissingData(aPrevFrame, aTransform.mFrame);
			ReanimationFillInMissingData(aPrevAlpha, aTransform.mAlpha);
			ReanimationFillInMissingData((void*&)aPrevImage, (void*&)aTransform.mImage);
			ReanimationFillInMissingData((void*&)aPrevFont, (void*&)aTransform.mFont);
			if (*aTransform.mText == '\0')
				aTransform.mText = aPrevText;
			else
				aPrevText = aTransform.mText;
		}
	}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	for (int aTrackIndex = 0; aTrackIndex < theDefinition->mTracks.count; aTrackIndex++)
	{
		ReanimatorTrack* aTrack = &theDefinition->mTracks.tracks[aTrackIndex];
		if (strncasecmp(aTrack->mName, "attacher__", 10) != 0)
			continue;

		const char* aLastText = nullptr;
		for (int i = 0; i < aTrack->mTransforms.count; i++)
		{
			ReanimatorTransform& aTransform = aTrack->mTransforms.mTransforms[i];
			if (aTransform.mText != aLastText)
			{
				ReanimatorGetAttacherCached(aTransform);
				aLastText = aTransform.mText;
			}
		}
	}
#endif
	return true;
}

//0x4717D0
void ReanimationFreeDefinition(ReanimatorDefinition* theDefinition)
{
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	ReanimatorInvalidateBasePoseCache(theDefinition);
	for (unsigned int aCacheIndex = 0; aCacheIndex < REANIMATOR_ATTACHER_CACHE_SIZE; aCacheIndex++)
	{
		ReanimatorAttacherCache& aCache = gReanimatorAttacherCache[aCacheIndex];
		if (!aCache.mValid)
			continue;

		bool aBelongsToDefinition = false;
		for (int aTrackIndex = 0; aTrackIndex < theDefinition->mTracks.count && !aBelongsToDefinition; aTrackIndex++)
		{
			ReanimatorTrack* aTrack = &theDefinition->mTracks.tracks[aTrackIndex];
			for (int i = 0; i < aTrack->mTransforms.count; i++)
			{
				if (aTrack->mTransforms.mTransforms[i].mText == aCache.mSourceText)
				{
					aBelongsToDefinition = true;
					break;
				}
			}
		}

		if (aBelongsToDefinition)
		{
			aCache.mSourceText = nullptr;
			aCache.mValid = 0;
		}
	}
#endif

	// 释放 Atlas
	if (theDefinition->mReanimAtlas != nullptr)
	{
		theDefinition->mReanimAtlas->ReanimAtlasDispose();
		delete theDefinition->mReanimAtlas;
		theDefinition->mReanimAtlas = nullptr;
	}

	// 恢复定义数据
	for (int aTrackIndex = 0; aTrackIndex < theDefinition->mTracks.count; aTrackIndex++)
	{
		ReanimatorTrack* aTrack = &theDefinition->mTracks.tracks[aTrackIndex];
		const char* aPrevText = nullptr;
		for (int i = 0; i < aTrack->mTransforms.count; i++)
		{
			ReanimatorTransform& aTransform = aTrack->mTransforms.mTransforms[i];
			if (*aTransform.mText != '\0' && aTransform.mText == aPrevText)
				aTransform.mText = "";
			else
				aPrevText = aTransform.mText;
		}
	}

	// 释放定义
	DefinitionFreeMap(&gReanimatorDefMap, theDefinition);
}

//0x471890
ReanimatorTrackInstance::ReanimatorTrackInstance()
{
	mBlendCounter = 0;
	mBlendTime = 0;
	mShakeOverride = 0.0f;
	mShakeX = 0.0f;
	mShakeY = 0.0f;
	mAttachmentID = AttachmentID::ATTACHMENTID_NULL;
	mRenderGroup = RENDER_GROUP_NORMAL;
	mIgnoreClipRect = false;
	mImageOverride = nullptr;
	mTruncateDisappearingFrames = true;
	mTrackColor = Color::White;
	mIgnoreColorOverride = false;
	mIgnoreExtraAdditiveColor = false;
}

//0x471920
Reanimation::Reanimation()
{
	mAnimTime = 0;
	mAnimRate = 12.0f;
	mDefinition = nullptr;
	mLoopType = ReanimLoopType::REANIM_PLAY_ONCE;
	mLastFrameTime = -1.0f;
	// An object is not live until ReanimationInitialize finishes.  Keeping a
	// freshly constructed instance dead makes stack unwinding safe if loading
	// a definition or building its atlas throws.
	mDead = true;
	mFrameStart = 0;
	mFrameCount = 0;
	mFrameBasePose = -1;
	mOverlayMatrix.LoadIdentity();
	mColorOverride = Color::White;
	mExtraAdditiveColor = Color::White;
	mEnableExtraAdditiveDraw = false;
	mExtraOverlayColor = Color::White;
	mEnableExtraOverlayDraw = false;
	mLoopCount = 0;
	mIsAttachment = false;
	mRenderOrder = 0;
	mReanimationHolder = nullptr;
	mTrackInstances = nullptr;
	mFilterEffect = FilterEffect::FILTER_EFFECT_NONE;
	mReanimationType = ReanimationType::REANIM_NONE;
}

//0x471A20
Reanimation::~Reanimation()
{
	ReanimationDie();
	ReanimationDelete();
}

void Reanimation::ReanimationDelete()
{
	TOD_ASSERT(mDead);
	if (mTrackInstances != nullptr)
	{
		int aItemSize = mDefinition->mTracks.count * sizeof(ReanimatorTrackInstance);
		FindGlobalAllocator(aItemSize)->Free(mTrackInstances, aItemSize);  // 由 TodAllocator 回收动画轨道的内存区域
		mTrackInstances = nullptr;
	}
}

//0x471A60
void Reanimation::ReanimationInitializeType(float theX, float theY, ReanimationType theReanimType)
{
	TOD_ASSERT(theReanimType >= 0 && theReanimType < gReanimatorDefCount);
	ReanimatorEnsureDefinitionLoaded(theReanimType, false);
	mReanimationType = theReanimType;
	ReanimationInitialize(theX, theY, &gReanimatorDefArray[(int)theReanimType]);
}

//0x471A90
void ReanimationCreateAtlas(ReanimatorDefinition* theDefinition, ReanimationType theReanimationType)
{
	ReanimationParams& aParam = gReanimationParamArray[(int)theReanimationType];
	if (theDefinition->mReanimAtlas != nullptr || TestBit(aParam.mReanimParamFlags, ReanimFlags::REANIM_NO_ATLAS))
		return;  // 当动画已存在 Atlas 或无需 Atlas 时，直接退出

#ifdef WII_PLATFORM
	// Runtime reanimation atlases are generated surfaces with no file backing.
	// Their construction pixels now use the console scratch pool, but the final
	// Wii texture still needs contiguous graphics-arena space. Drawing without
	// an atlas is already supported, so avoid creating another persistent atlas
	// when the texture arena is too fragmented to make it worthwhile.
	const size_t aLargestGraphicsBlock = ConsoleGraphicsLargestFreeBytes();
	if (aLargestGraphicsBlock != 0 && aLargestGraphicsBlock < (2u << 20))
	{
		static unsigned int sAtlasSkipCount = 0;
		sAtlasSkipCount++;
		if (sAtlasSkipCount <= 8 || (sAtlasSkipCount & (sAtlasSkipCount - 1)) == 0)
			printf("[WII][ATLAS] skip under pressure: largest=%uKB type=%d count=%u\n",
				(unsigned)(aLargestGraphicsBlock / 1024), (int)theReanimationType, sAtlasSkipCount);
		return;
	}
#endif

#ifdef PS2_PLATFORM
	// PS2 has the same direct-image fallback, but its pressure signal comes
	// from the EE heap and reserved big-block pool rather than Wii/MEM2.
	if (Ps2ShouldPurgeAssets() && ConsoleBigBlockLargestFreeBytes() < (512u << 10))
	{
		static unsigned int sPs2AtlasSkipCount = 0;
		sPs2AtlasSkipCount++;
		if (sPs2AtlasSkipCount <= 8 || (sPs2AtlasSkipCount & (sPs2AtlasSkipCount - 1)) == 0)
			printf("[PS2][ATLAS] skip under pressure: bigLargest=%uKB type=%d count=%u\n",
				(unsigned)(ConsoleBigBlockLargestFreeBytes() / 1024), (int)theReanimationType, sPs2AtlasSkipCount);
		return;
	}
#endif

	PerfTimer aTimer;
	aTimer.Start();
	TodHesitationTrace("preatlas");
	ReanimAtlas* aAtlas = new ReanimAtlas();
	if (!aAtlas->ReanimAtlasCreate(theDefinition))
	{
		delete aAtlas;
		return;
	}
	theDefinition->mReanimAtlas = aAtlas;  // 赋值动画 Atlas 指针

#ifdef LOW_MEMORY
	// The atlas just copied every source part into its own surface; the
	// originals' decoded bits are dead weight from here on (they are only
	// needed again if this atlas is rebuilt, and they re-decode on demand).
	for (int i = 0; i < aAtlas->mImageCount; i++)
	{
		MemoryImage* aOriginal = (MemoryImage*)aAtlas->mImageArray[i].mOriginalImage;
		if (aOriginal != nullptr)
			gSexyAppBase->PurgeLazyImage(aOriginal, true);
	}

	if (aAtlas->mMemoryImage != nullptr)
	{
#ifdef WII_PLATFORM
		// The Almanac first draws a live Sunflower/Zombie on its index page,
		// then builds the small selector thumbnails in software from the same
		// atlas. Wii/OpenGX has no glGetTexImage: purging the atlas after that
		// first GPU upload made the later software draw reconstruct an all-zero
		// surface, which the cache correctly reported as "blank; will retry".
		//
		// Keep a recoverable CPU copy, but palletize it now so it costs roughly
		// one byte per pixel rather than four while it is only GPU-resident. A
		// later software blit expands it temporarily; the normal per-thumbnail
		// definition cleanup then releases the whole atlas.
		aAtlas->mMemoryImage->Palletize();
		aAtlas->mMemoryImage->mPurgeBits = false;
#else
		// Other accelerated backends can recover the surface from their texture
		// copy, so they may still discard the duplicate after the first upload.
		aAtlas->mMemoryImage->mPurgeBits = true;
#endif
	}
#endif

#ifdef PS2_PLATFORM
	// Commit right here instead of waiting for this atlas's first natural
	// draw. That matters for both callers of this function: a preload burst
	// (the credits screen stacks ~35 atlases) would otherwise accumulate
	// every surface's full 32bpp bits at once, and an on-demand build mid-
	// gameplay (ReanimationInitialize, e.g. the first zombie of a new type
	// spawning in a wave) would otherwise keep its surface alive through the
	// rest of the current frame's render-list build/sort/draw — exactly
	// when the heap is already busiest and a big contiguous alloc is least
	// likely to succeed. The shim keeps only its quarter-size palettized
	// copy; GS VRAM residency stays deferred to the first bind. The texture
	// table is primary-thread-only, so an off-thread build (loading thread)
	// keeps the old commit-on-first-draw behavior.
	if (aAtlas->mMemoryImage != nullptr && gSexyAppBase->mGLInterface != nullptr &&
		(gSexyAppBase->mPrimaryThreadId == 0 ||
		 (void*)pthread_self() == gSexyAppBase->mPrimaryThreadId))
	{
		gSexyAppBase->mGLInterface->CreateImageTexture(aAtlas->mMemoryImage);
	}
#endif

	TodHesitationTrace("atlas '%s'", aParam.mReanimFileName);
	int aDuration = std::max(aTimer.GetDuration(), 0.0);
	if (aDuration > 20 && theReanimationType != ReanimationType::REANIM_NONE)  //（仅内测版）创建时间过长的报告
		TodTraceAndLog("LOADING:Long atlas '%s' %d ms on %s", aParam.mReanimFileName, aDuration, gGetCurrentLevelName().c_str());
}

void ReanimationPreload(ReanimationType theReanimationType)
{
	TOD_ASSERT(theReanimationType >= 0 && theReanimationType < gReanimatorDefCount);

	ReanimatorDefinition* aReanimDef = &gReanimatorDefArray[(int)theReanimationType];
	ReanimationCreateAtlas(aReanimDef, theReanimationType);
	if (aReanimDef->mReanimAtlas && aReanimDef->mReanimAtlas->mMemoryImage != nullptr)
	{
		// The PS2 immediate-commit-and-drop-bits step lives in
		// ReanimationCreateAtlas itself now, so it also covers the on-demand
		// build from ReanimationInitialize (see the comment there). Empty
		// text-only atlases deliberately have no backing MemoryImage.
		TodSandImageIfNeeded(aReanimDef->mReanimAtlas->mMemoryImage);
	}
}

//0x471B00
void Reanimation::ReanimationInitialize(float theX, float theY, ReanimatorDefinition* theDefinition)
{
	TOD_ASSERT(mTrackInstances == nullptr);
	TOD_ASSERT(mDead);
	mDefinition = theDefinition;
	ReanimationCreateAtlas(theDefinition, mReanimationType);
	SetPosition(theX, theY);
	mAnimRate = theDefinition->mFPS;
	mLastFrameTime = -1.0f;

	if (theDefinition->mTracks.count != 0)
	{
		mFrameCount = mDefinition->mTracks.tracks[0].mTransforms.count;
		int aItemSize = theDefinition->mTracks.count * sizeof(ReanimatorTrackInstance);
		mTrackInstances = (ReanimatorTrackInstance*)FindGlobalAllocator(aItemSize)->Calloc(aItemSize);  // 申请动画轨道实例数组所需的内存
		for (int aTrackIndex = 0; aTrackIndex < mDefinition->mTracks.count; aTrackIndex++)  // 遍历初始化数组中每个轨道实例
		{
			ReanimatorTrackInstance* aTrack = &mTrackInstances[aTrackIndex];
			if (aTrack != nullptr)
				new (aTrack)ReanimatorTrackInstance();
		}
	}
	else
		mFrameCount = 0;

	// Publish the reanimation as live only after every potentially throwing
	// allocation and track construction has completed.
	mDead = false;
}

//0x471BC0
// GOTY @Patoke: 0x4761C0
void Reanimation::Update()
{
	if (mFrameCount == 0 || mDead)
		return;

	TOD_ASSERT(std::isfinite(mAnimRate));
	mLastFrameTime = mAnimTime;  // 更新上一帧的循环率
	mAnimTime += SECONDS_PER_UPDATE * mAnimRate / mFrameCount;  // 更新当前循环率

	if (mAnimRate > 0)
	{
		switch (mLoopType)
		{
		case ReanimLoopType::REANIM_LOOP:
		case ReanimLoopType::REANIM_LOOP_FULL_LAST_FRAME:
			while (mAnimTime >= 1.0f)
			{
				mLoopCount++;
				mAnimTime -= 1.0f;
			}
			break;
		case ReanimLoopType::REANIM_PLAY_ONCE:
		case ReanimLoopType::REANIM_PLAY_ONCE_FULL_LAST_FRAME:
			if (mAnimTime >= 1.0f)
			{
				mLoopCount = 1;
				mAnimTime = 1.0f;
				mDead = true;
			}
			break;
		case ReanimLoopType::REANIM_PLAY_ONCE_AND_HOLD:
		case ReanimLoopType::REANIM_PLAY_ONCE_FULL_LAST_FRAME_AND_HOLD:
			if (mAnimTime >= 1.0f)
			{
				mLoopCount = 1;
				mAnimTime = 1.0f;
			}
			break;
		default:
			TOD_ASSERT(false);
			break;
		}
	}
	else
	{
		switch (mLoopType)
		{
		case ReanimLoopType::REANIM_LOOP:
		case ReanimLoopType::REANIM_LOOP_FULL_LAST_FRAME:
			while (mAnimTime < 0.0f)
			{
				mLoopCount++;
				mAnimTime += 1.0f;
			}
			break;
		case ReanimLoopType::REANIM_PLAY_ONCE:
		case ReanimLoopType::REANIM_PLAY_ONCE_FULL_LAST_FRAME:
			if (mAnimTime < 0.0f)
			{
				mLoopCount = 1;
				mAnimTime = 0.0f;
				mDead = true;
			}
			break;
		case ReanimLoopType::REANIM_PLAY_ONCE_AND_HOLD:
		case ReanimLoopType::REANIM_PLAY_ONCE_FULL_LAST_FRAME_AND_HOLD:
			if (mAnimTime < 0.0f)
			{
				mLoopCount = 1;
				mAnimTime = 0.0f;
			}
			break;
		default:
			TOD_ASSERT(false);
			break;
		}
	}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	ReanimatorFrameTime aUpdateFrameTime;
	if (mDefinition->mTracks.count > 0)
		GetFrameTime(&aUpdateFrameTime);
#endif

	for (int aTrackIndex = 0; aTrackIndex < mDefinition->mTracks.count; aTrackIndex++)
	{
		ReanimatorTrackInstance* aTrack = &mTrackInstances[aTrackIndex];
		if (aTrack->mBlendCounter > 0)
			aTrack->mBlendCounter--;  // 更新轨道的混合倒计时

		if (aTrack->mShakeOverride != 0.0f)  // 更新轨道震动
		{
			aTrack->mShakeX = RandRangeFloat(-aTrack->mShakeOverride, aTrack->mShakeOverride);
			aTrack->mShakeY = RandRangeFloat(-aTrack->mShakeOverride, aTrack->mShakeOverride);
		}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
		bool aIsAttacher = strncasecmp(mDefinition->mTracks.tracks[aTrackIndex].mName, "attacher__", 10) == 0;
		bool aNeedsTransform = aIsAttacher || aTrack->mAttachmentID != AttachmentID::ATTACHMENTID_NULL;
		ReanimatorTransform aTransform;
		if (aNeedsTransform)
			GetCurrentTransformAtTime(aTrackIndex, &aTransform, &aUpdateFrameTime);

		if (aIsAttacher)
			UpdateAttacherTrack(aTrackIndex, &aTransform);

		if (aTrack->mAttachmentID != AttachmentID::ATTACHMENTID_NULL)
		{
			SexyTransform2D aOverlayMatrix;
			GetAttachmentOverlayMatrix(aTrackIndex, aOverlayMatrix, &aTransform);
			AttachmentUpdateAndSetMatrix(aTrack->mAttachmentID, aOverlayMatrix);
		}
#else
		if (strncasecmp(mDefinition->mTracks.tracks[aTrackIndex].mName, "attacher__", 10) == 0)  // IsAttacher
			UpdateAttacherTrack(aTrackIndex);

		if (aTrack->mAttachmentID != AttachmentID::ATTACHMENTID_NULL)
		{
			SexyTransform2D aOverlayMatrix;
			GetAttachmentOverlayMatrix(aTrackIndex, aOverlayMatrix);
			AttachmentUpdateAndSetMatrix(aTrack->mAttachmentID, aOverlayMatrix);
		}
#endif
	}
}

//0x471E50
void BlendTransform(ReanimatorTransform* theResult, const ReanimatorTransform& theTransform1, const ReanimatorTransform& theTransform2, float theBlendFactor)
{
	theResult->mTransX = FloatLerp(theTransform1.mTransX, theTransform2.mTransX, theBlendFactor);
	theResult->mTransY = FloatLerp(theTransform1.mTransY, theTransform2.mTransY, theBlendFactor);
	theResult->mScaleX = FloatLerp(theTransform1.mScaleX, theTransform2.mScaleX, theBlendFactor);
	theResult->mScaleY = FloatLerp(theTransform1.mScaleY, theTransform2.mScaleY, theBlendFactor);
	theResult->mAlpha = FloatLerp(theTransform1.mAlpha, theTransform2.mAlpha, theBlendFactor);

	float aSkewX2 = theTransform2.mSkewX;
	float aSkewY2 = theTransform2.mSkewY;
	// 推测这里原意是为了确保从两个变换之间的倾斜角度不超过 π（WP 版），
	// 原版（以及内测版）实际为，倾斜超过 π 时 theTransform2 变换无效
	while (aSkewX2 > theTransform1.mSkewX + 180.0f)
		aSkewX2 = theTransform1.mSkewX;  // （aSkewX2 -= 360.0f）
	while (aSkewX2 < theTransform1.mSkewX - 180.0f)
		aSkewX2 = theTransform1.mSkewX;  // （aSkewX2 += 360.0f）
	while (aSkewY2 > theTransform1.mSkewY + 180.0f)
		aSkewY2 = theTransform1.mSkewY;  // （aSkewY2 -= 360.0f）
	while (aSkewY2 < theTransform1.mSkewY - 180.0f)
		aSkewY2 = theTransform1.mSkewY;  // （aSkewY2 += 360.0f）

	theResult->mSkewX = FloatLerp(theTransform1.mSkewX, aSkewX2, theBlendFactor);
	theResult->mSkewY = FloatLerp(theTransform1.mSkewY, aSkewY2, theBlendFactor);
	theResult->mFrame = theTransform1.mFrame;
	theResult->mFont = theTransform1.mFont;
	theResult->mText = theTransform1.mText;
	theResult->mImage = theTransform1.mImage;
}

//0x471F90
// GOTY @Patoke: 0x476580
void Reanimation::GetCurrentTransform(int theTrackIndex, ReanimatorTransform* theTransformCurrent)
{
	ReanimatorFrameTime aFrameTime;
	GetFrameTime(&aFrameTime);
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	GetCurrentTransformAtTime(theTrackIndex, theTransformCurrent, &aFrameTime);
#else
	GetTransformAtTime(theTrackIndex, theTransformCurrent, &aFrameTime);  // 结合两帧之间的自然补间取得基础变换

	ReanimatorTrackInstance* aTrack = &mTrackInstances[theTrackIndex];
	if (FloatRoundToInt(theTransformCurrent->mFrame) >= 0 && aTrack->mBlendCounter > 0)  // 若当前不为空白帧且轨道处于变换混合过程中
	{
		float aBlendFactor = aTrack->mBlendCounter / (float)aTrack->mBlendTime;
		BlendTransform(theTransformCurrent, *theTransformCurrent, aTrack->mBlendTransform, aBlendFactor);  // 结合覆写变换计算混合后的实际变换
	}
#endif
}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
void Reanimation::GetCurrentTransformAtTime(int theTrackIndex, ReanimatorTransform* theTransformCurrent, ReanimatorFrameTime* theFrameTime)
{
	GetTransformAtTime(theTrackIndex, theTransformCurrent, theFrameTime);

	ReanimatorTrackInstance* aTrack = &mTrackInstances[theTrackIndex];
	if (FloatRoundToInt(theTransformCurrent->mFrame) >= 0 && aTrack->mBlendCounter > 0)
	{
		float aBlendFactor = aTrack->mBlendCounter / (float)aTrack->mBlendTime;
		BlendTransform(theTransformCurrent, *theTransformCurrent, aTrack->mBlendTransform, aBlendFactor);
	}
}
#endif

//0x472020
void Reanimation::GetTransformAtTime(int theTrackIndex, ReanimatorTransform* theTransform, ReanimatorFrameTime* theFrameTime)
{
	TOD_ASSERT(theTrackIndex >= 0 && theTrackIndex < mDefinition->mTracks.count);
	ReanimatorTrack* aTrack = &mDefinition->mTracks.tracks[theTrackIndex];
	TOD_ASSERT(aTrack->mTransforms.count == mDefinition->mTracks.tracks[0].mTransforms.count);
	ReanimatorTransform& aTransBefore = aTrack->mTransforms.mTransforms[theFrameTime->mAnimFrameBeforeInt];  // 前一帧的变换定义
	ReanimatorTransform& aTransAfter = aTrack->mTransforms.mTransforms[theFrameTime->mAnimFrameAfterInt];  // 后一帧的变换定义

	theTransform->mTransX = FloatLerp(aTransBefore.mTransX, aTransAfter.mTransX, theFrameTime->mFraction);
	theTransform->mTransY = FloatLerp(aTransBefore.mTransY, aTransAfter.mTransY, theFrameTime->mFraction);
	theTransform->mSkewX = FloatLerp(aTransBefore.mSkewX, aTransAfter.mSkewX, theFrameTime->mFraction);
	theTransform->mSkewY = FloatLerp(aTransBefore.mSkewY, aTransAfter.mSkewY, theFrameTime->mFraction);
	theTransform->mScaleX = FloatLerp(aTransBefore.mScaleX, aTransAfter.mScaleX, theFrameTime->mFraction);
	theTransform->mScaleY = FloatLerp(aTransBefore.mScaleY, aTransAfter.mScaleY, theFrameTime->mFraction);
	theTransform->mAlpha = FloatLerp(aTransBefore.mAlpha, aTransAfter.mAlpha, theFrameTime->mFraction);
	theTransform->mImage = aTransBefore.mImage;
	theTransform->mFont = aTransBefore.mFont;
	theTransform->mText = aTransBefore.mText;

	if (aTransBefore.mFrame != -1.0f && aTransAfter.mFrame == -1.0f && theFrameTime->mFraction > 0.0f && mTrackInstances[theTrackIndex].mTruncateDisappearingFrames)
		theTransform->mFrame = -1.0f;  // 当从一个非空白帧过渡至空白帧时，若轨道设置了截断消失帧，则删去过渡的过程
	else
		theTransform->mFrame = aTransBefore.mFrame;
}

//0x4720F0
void Reanimation::MatrixFromTransform(const ReanimatorTransform& theTransform, SexyMatrix3& theMatrix)
{
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	if (theTransform.mSkewX == 0.0f && theTransform.mSkewY == 0.0f)
	{
		theMatrix.m00 = theTransform.mScaleX;
		theMatrix.m10 = 0.0f;
		theMatrix.m01 = 0.0f;
		theMatrix.m11 = theTransform.mScaleY;
	}
	else if (theTransform.mSkewX == theTransform.mSkewY)
	{
		float aSkew = -DEG_TO_RAD(theTransform.mSkewX);
		float aSin = sinf(aSkew);
		float aCos = cosf(aSkew);
		theMatrix.m00 = aCos * theTransform.mScaleX;
		theMatrix.m10 = -aSin * theTransform.mScaleX;
		theMatrix.m01 = aSin * theTransform.mScaleY;
		theMatrix.m11 = aCos * theTransform.mScaleY;
	}
	else
	{
		float aSkewX = -DEG_TO_RAD(theTransform.mSkewX);
		float aSkewY = -DEG_TO_RAD(theTransform.mSkewY);
		float aSinX = sinf(aSkewX);
		float aCosX = cosf(aSkewX);
		float aSinY = sinf(aSkewY);
		float aCosY = cosf(aSkewY);
		theMatrix.m00 = aCosX * theTransform.mScaleX;
		theMatrix.m10 = -aSinX * theTransform.mScaleX;
		theMatrix.m01 = aSinY * theTransform.mScaleY;
		theMatrix.m11 = aCosY * theTransform.mScaleY;
	}
	theMatrix.m20 = 0.0f;
#else
	// 将倾斜的角度转化为弧度
	float aSkewX = -DEG_TO_RAD(theTransform.mSkewX);
	float aSkewY = -DEG_TO_RAD(theTransform.mSkewY);
	theMatrix.m00 = cos(aSkewX) * theTransform.mScaleX;
	theMatrix.m10 = -sin(aSkewX) * theTransform.mScaleX;
	theMatrix.m20 = 0.0f;
	theMatrix.m01 = sin(aSkewY) * theTransform.mScaleY;
	theMatrix.m11 = cos(aSkewY) * theTransform.mScaleY;
#endif
	theMatrix.m21 = 0.0f;
	theMatrix.m02 = theTransform.mTransX;
	theMatrix.m12 = theTransform.mTransY;
	theMatrix.m22 = 1.0f;
}

//0x472190
void Reanimation::ReanimBltMatrix(Graphics* g, Image* theImage, SexyMatrix3& theTransform, const Rect& theClipRect, const Color& theColor, int theDrawMode, const Rect& theSrcRect)
{
	if (!gSexyAppBase->Is3DAccelerated() &&  // 未开启 3D 硬件加速
		TestBit(gReanimationParamArray[(int)mReanimationType].mReanimParamFlags, (int)ReanimFlags::REANIM_FAST_DRAW_IN_SW_MODE) &&  // 动画允许使用软件渲染
		FloatApproxEqual(theTransform.m01, 0.0f) && FloatApproxEqual(theTransform.m10, 0.0f) &&  // 横向和纵向的倾斜值均为 0
		theTransform.m00 > 0.0f && theTransform.m11 > 0.0f &&  // 横向和纵向的拉伸值均大于 0
		theColor == Color::White)
	{
		float aScaleX = theTransform.m00;
		float aScaleY = theTransform.m11;
		int aPosX = FloatRoundToInt(theTransform.m02 - aScaleX * theSrcRect.mWidth * 0.5f);
		int aPosY = FloatRoundToInt(theTransform.m12 - aScaleY * theSrcRect.mHeight * 0.5f);
		int aOldMode = g->GetDrawMode();  // 备份原绘制模式
		g->SetDrawMode(theDrawMode);
		Rect aOldClipRect = g->mClipRect;  // 备份原裁剪矩形
		g->SetClipRect(theClipRect);

		if (FloatApproxEqual(aScaleX, 1.0f) && FloatApproxEqual(aScaleY, 1.0f))  // 如果无拉伸
			g->DrawImage(theImage, aPosX, aPosY, theSrcRect);
		else
		{
			int aWidth = FloatRoundToInt(aScaleX * theSrcRect.mWidth);
			int aHeight = FloatRoundToInt(aScaleY * theSrcRect.mHeight);
			Rect aDestRect(aPosX, aPosY, aWidth, aHeight);
			g->DrawImage(theImage, aDestRect, theSrcRect);
		}

		g->SetDrawMode(aOldMode);  // 还原绘制模式
		g->SetClipRect(aOldClipRect);  // 还原裁剪矩形
	}
	else
		TodBltMatrix(g, theImage, theTransform, theClipRect, theColor, theDrawMode, theSrcRect);
}

//0x4723B0
// GOTY @Patoke: 0x4769B0
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
bool Reanimation::DrawTrackAtTime(Graphics* g, int theTrackIndex, int theRenderGroup, TodTriangleGroup* theTriangleGroup, ReanimatorFrameTime* theFrameTime)
#else
bool Reanimation::DrawTrack(Graphics* g, int theTrackIndex, int theRenderGroup, TodTriangleGroup* theTriangleGroup)
#endif
{
	(void)theRenderGroup;
	ReanimatorTransform aTransform;
	ReanimatorTrackInstance* aTrackInstance = &mTrackInstances[theTrackIndex];  // 目标轨道的指针
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	if (theFrameTime != nullptr)
		GetCurrentTransformAtTime(theTrackIndex, &aTransform, theFrameTime);
	else
#endif
		GetCurrentTransform(theTrackIndex, &aTransform);  // 取得当前动画变换
	int aImageFrame = FloatRoundToInt(aTransform.mFrame);  // 图像在贴图中所处的份数
	if (aImageFrame < 0)  // 不存在图像时，返回
		return false;

	Color aColor = aTrackInstance->mTrackColor;
	if (!aTrackInstance->mIgnoreColorOverride)  // 除非轨道无视动画的覆写颜色
	{
		aColor = ColorsMultiply(aColor, mColorOverride);  // 将轨道颜色与动画的覆写颜色进行正片叠底混合
	}
	if (g->GetColorizeImages())  // 若 Graphics 着色
	{
		aColor = ColorsMultiply(aColor, g->GetColor());  // 将颜色再与 Graphics 的颜色进行正片叠底混合
	}
	int aImageAlpha = ClampInt(FloatRoundToInt(aTransform.mAlpha * aColor.mAlpha), 0, 255);
	if (aImageAlpha <= 0)  // 当图像完全透明时，返回
	{
		return false;
	}
	aColor.mAlpha = aImageAlpha;

	Color aExtraAdditiveColor;
	if (mEnableExtraAdditiveDraw)  // 如果动画启用额外叠加颜色（高亮）
	{
		aExtraAdditiveColor = mExtraAdditiveColor;
		aExtraAdditiveColor.mAlpha = ColorComponentMultiply(mExtraAdditiveColor.mAlpha, aImageAlpha);
	}
	Color aExtraOverlayColor;
	if (mEnableExtraOverlayDraw)
	{
		aExtraOverlayColor = mExtraOverlayColor;
		aExtraOverlayColor.mAlpha = ColorComponentMultiply(mExtraOverlayColor.mAlpha, aImageAlpha);
	}

	Rect aClipRect = g->mClipRect;
	if (aTrackInstance->mIgnoreClipRect)  // 如果轨道无视裁剪矩形
	{
		aClipRect = Rect(0, 0, BOARD_WIDTH, BOARD_HEIGHT);  // 裁剪矩形重置为屏幕矩形
	}

	Image* aImage = aTransform.mImage;
	if (aTrackInstance->mImageOverride == IMAGE_BLANK)
		return false;
	ReanimAtlasImage* aAtlasImage = nullptr;
	if (mDefinition->mReanimAtlas != nullptr && aImage != nullptr)  // 如果 atlas 存在且当前变换存在图像（aTransform.mImage 实际为整数型的图集编号）
	{
		aAtlasImage = mDefinition->mReanimAtlas->GetEncodedReanimAtlas(aImage);  // 取得相应的图集数据
		if (aAtlasImage != nullptr)  // 如果是合法的图集编号，成功取得对应指针
		{
			aImage = aAtlasImage->mOriginalImage;  // 将真正的 Sexy::Image* 类型的贴图赋值给 aImage
		}
		if (aTrackInstance->mImageOverride != nullptr)  // 如果目标轨道存在覆写贴图
		{
			aAtlasImage = nullptr;  // 不使用图集
		}
		else if (aAtlasImage == nullptr && (uintptr_t)aImage <= 1000)
		{
			// Atlas-local index copied from a different definition.  It cannot
			// be decoded safely here, so omit this one track image.
			aImage = nullptr;
		}
	}
	SexyMatrix3 aMatrix;
	bool aFullScreen = false;
	if (aImage != nullptr)  // 如果存在贴图。此处若上一步中图集编号非法，则可能导致崩溃
	{
		int aCelWidth = aImage->GetCelWidth();
		int aCelHeight = aImage->GetCelHeight();
		aMatrix.LoadIdentity();
		SexyMatrix3Translation(aMatrix, aCelWidth * 0.5f, aCelHeight * 0.5f);  // 将矩阵变换的坐标设定在贴图的中心位置
	}
	else if (aTransform.mFont != nullptr && *aTransform.mText != '\0')  // 如果存在字体且文本不为空
	{
		aMatrix.LoadIdentity();
		int aWidth = aTransform.mFont->StringWidth(aTransform.mText);
		SexyMatrix3Translation(aMatrix, -aWidth * 0.5f, aTransform.mFont->mAscent);
	}
	else
	{
		if (strcasecmp(mDefinition->mTracks.tracks[theTrackIndex].mName, "fullscreen"))  // 如果既没有图像也没有文本，且不是全屏轨道
			return false;  // 无需绘制
		aFullScreen = true;  // 标记全屏轨道，后续会填充一个屏幕大小的矩形
	}

	if (mDefinition->mReanimAtlas != nullptr && aAtlasImage == nullptr)  // 有 atlas 但不用的情况
		theTriangleGroup->DrawGroup(g);  // 先把原有的三角组绘制了

	SexyMatrix3 aTransformMatrix;
	MatrixFromTransform(aTransform, aTransformMatrix);
	SexyMatrix3Multiply(aMatrix, aTransformMatrix, aMatrix);  // 以动画变换矩阵作用 aMatrix
	SexyMatrix3Multiply(aMatrix, mOverlayMatrix, aMatrix);  // 以动画覆写矩阵作用 aMatrix
	SexyMatrix3Translation(aMatrix, aTrackInstance->mShakeX + g->mTransX - 0.5f, aTrackInstance->mShakeY + g->mTransY - 0.5f);  // 轨道震动及 g 的影响

	if (aAtlasImage != nullptr)  // 如果存在图集（动画定义存在 atlas，轨道变换存在图像，轨道不存在覆写贴图）
	{
		Rect aSrcRect(aAtlasImage->mX, aAtlasImage->mY, aAtlasImage->mWidth, aAtlasImage->mHeight);
		aImage = mDefinition->mReanimAtlas->mMemoryImage;
		if (mFilterEffect != FilterEffect::FILTER_EFFECT_NONE)  // 如果动画存在滤镜
		{
			aImage = FilterEffectGetImage(aImage, mFilterEffect);  // 取得滤镜后的贴图
		}
		theTriangleGroup->AddTriangle(g, aImage, aMatrix, aClipRect, aColor, g->mDrawMode, aSrcRect);  // 向三角组中添加三角形
		if (mEnableExtraAdditiveDraw && !aTrackInstance->mIgnoreExtraAdditiveColor)  // 如果动画存在额外叠加颜色且轨道不能无视之
		{
			theTriangleGroup->AddTriangle(g, aImage, aMatrix, aClipRect, aExtraAdditiveColor, Graphics::DRAWMODE_ADDITIVE, aSrcRect);
		}
		if (mEnableExtraOverlayDraw)
		{
			theTriangleGroup->AddTriangle(
				g, FilterEffectGetImage(aImage, FilterEffect::FILTER_EFFECT_WHITE), aMatrix, aClipRect, aExtraOverlayColor, Graphics::DRAWMODE_NORMAL, aSrcRect);
		}
	}
	else if (aImage != nullptr)  // 如果不存在 atlas 但轨道变换存在图像
	{
		if (aTrackInstance->mImageOverride != nullptr)  // 如果轨道存在覆写贴图
		{
			aImage = aTrackInstance->mImageOverride;  // 将贴图替换为覆写贴图
		}
		if (mFilterEffect != FilterEffect::FILTER_EFFECT_NONE)  // 如果动画存在滤镜
		{
			aImage = FilterEffectGetImage(aImage, mFilterEffect);  // 将贴图替换为滤镜后的贴图
		}
		while (aImageFrame >= aImage->mNumCols)
		{
			aImageFrame -= aImage->mNumCols;  // 确保绘制的列数不会超过贴图最后一列
		}

		int aCelWidth = aImage->GetCelWidth();
		Rect aSrcRect(aImageFrame * aCelWidth, 0, aCelWidth, aImage->GetCelHeight());
		ReanimBltMatrix(g, aImage, aMatrix, aClipRect, aColor, g->mDrawMode, aSrcRect);  // 带矩阵绘制轨道图像
		if (mEnableExtraAdditiveDraw)
		{
			ReanimBltMatrix(g, aImage, aMatrix, aClipRect, aExtraAdditiveColor, Graphics::DRAWMODE_ADDITIVE, aSrcRect);
		}
		if (mEnableExtraOverlayDraw)
		{
			Image* aOverlayImage = FilterEffectGetImage(aImage, FilterEffect::FILTER_EFFECT_WHITE);
			ReanimBltMatrix(g, aOverlayImage, aMatrix, aClipRect, aExtraOverlayColor, Graphics::DRAWMODE_NORMAL, aSrcRect);
		}
	}
	else if (aTransform.mFont != nullptr && *aTransform.mText != '\0')  // 如果不存在图像但存在文本
	{
		TodDrawStringMatrix(g, aTransform.mFont, aMatrix, aTransform.mText, aColor);
		if (mEnableExtraAdditiveDraw)
		{
			int aOldMode = g->GetDrawMode();  // 备份绘制模式
			g->SetDrawMode(Graphics::DRAWMODE_ADDITIVE);
			TodDrawStringMatrix(g, aTransform.mFont, aMatrix, aTransform.mText, aExtraAdditiveColor);
			g->SetDrawMode(aOldMode);  // 还原绘制模式
		}
	}
	else if (aFullScreen)  // 不存在图像和文本，但是全屏
	{
		Color aOldColor = g->GetColor();  // 备份颜色
		g->SetColor(aColor);
		g->FillRect(-g->mTransX, -g->mTransY, BOARD_WIDTH, BOARD_HEIGHT);
		g->SetColor(aOldColor);  // 还原颜色
	}
	return true;
}

//0x472B70
Image* Reanimation::GetCurrentTrackImage(const char* theTrackName)
{
	int aTrackIndex = FindTrackIndex(theTrackName);
	ReanimatorTransform aTransform;
	GetCurrentTransform(aTrackIndex, &aTransform);

	Image* aImage = aTransform.mImage;
	if (mDefinition->mReanimAtlas != nullptr && aImage != nullptr)  // 如果存在图集且存在图像（否则返回的 aImage 为 nullptr）
	{
		ReanimAtlasImage* aAtlasImage = mDefinition->mReanimAtlas->GetEncodedReanimAtlas(aImage);  // 取得相应的图集数据
		if (aAtlasImage != nullptr)
			aImage = aAtlasImage->mOriginalImage;  // 返回图集对应的原贴图
		else if ((uintptr_t)aImage <= 1000)
			aImage = nullptr;
	}
	return aImage;
}

//0x472C00
void Reanimation::GetTrackMatrix(int theTrackIndex, SexyTransform2D& theMatrix)
{
	ReanimatorTrackInstance* aTrackInstance = &mTrackInstances[theTrackIndex];
	ReanimatorTransform aTransform;
	GetCurrentTransform(theTrackIndex, &aTransform);
	int aImageFrame = FloatRoundToInt(aTransform.mFrame);
	Image* aImage = aTransform.mImage;
	if (mDefinition->mReanimAtlas != nullptr && aImage != nullptr)  // 如果存在图集且存在图像（否则返回的 aImage 为 nullptr）
	{
		ReanimAtlasImage* aAtlasImage = mDefinition->mReanimAtlas->GetEncodedReanimAtlas(aImage);  // 取得相应的图集数据
		if (aAtlasImage != nullptr)
			aImage = aAtlasImage->mOriginalImage;  // 返回图集对应的原贴图
		else if ((uintptr_t)aImage <= 1000)
			aImage = nullptr;
	}

	theMatrix.LoadIdentity();
	if (aImage != nullptr && aImageFrame >= 0)
	{
		int aCelWidth = aImage->GetCelWidth();
		int aCelHeight = aImage->GetCelHeight();
		SexyMatrix3Translation(theMatrix, aCelWidth * 0.5f, aCelHeight * 0.5f);  // 将矩阵变换的坐标设定在贴图的中心位置
	}
	else if (aTransform.mFont != nullptr && *aTransform.mText != '\0')
		SexyMatrix3Translation(theMatrix, 0.0f, aTransform.mFont->mAscent);

	SexyTransform2D aTransformMatrix;
	MatrixFromTransform(aTransform, aTransformMatrix);
	SexyMatrix3Multiply(theMatrix, aTransformMatrix, theMatrix);  // 以动画变换矩阵作用 theMatrix
	SexyMatrix3Multiply(theMatrix, mOverlayMatrix, theMatrix);  // 以动画覆写矩阵作用 theMatrix
	SexyMatrix3Translation(theMatrix, aTrackInstance->mShakeX - 0.5f, aTrackInstance->mShakeY - 0.5f);  // 轨道震动的影响
}

//0x472D90
void Reanimation::GetFrameTime(ReanimatorFrameTime* theFrameTime)
{
	TOD_ASSERT(mFrameStart + mFrameCount <= mDefinition->mTracks.tracks[0].mTransforms.count);
	int aFrameCount;
	if (mLoopType == ReanimLoopType::REANIM_PLAY_ONCE_FULL_LAST_FRAME || mLoopType == ReanimLoopType::REANIM_LOOP_FULL_LAST_FRAME ||
		mLoopType == ReanimLoopType::REANIM_PLAY_ONCE_FULL_LAST_FRAME_AND_HOLD)
		aFrameCount = mFrameCount;
	else
		aFrameCount = mFrameCount - 1;
	float aAnimPosition = mFrameStart + mAnimTime * aFrameCount;
	float aAnimFrameBefore = floor(aAnimPosition);
	theFrameTime->mFraction = aAnimPosition - aAnimFrameBefore;
	theFrameTime->mAnimFrameBeforeInt = FloatRoundToInt(aAnimFrameBefore);
	if (theFrameTime->mAnimFrameBeforeInt >= mFrameStart + mFrameCount - 1)  // 如果当前处于结束的一帧
	{
		theFrameTime->mAnimFrameBeforeInt = mFrameStart + mFrameCount - 1;
		theFrameTime->mAnimFrameAfterInt = theFrameTime->mAnimFrameBeforeInt;  // 将前、后的整数帧均赋值为最后一帧
	}
	else
		theFrameTime->mAnimFrameAfterInt = theFrameTime->mAnimFrameBeforeInt + 1;  // 后一整数帧等于前一整数帧的后一帧
	TOD_ASSERT(theFrameTime->mAnimFrameBeforeInt >= 0 && theFrameTime->mAnimFrameAfterInt < mDefinition->mTracks.tracks[0].mTransforms.count);
}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
bool Reanimation::DrawTrack(Graphics* g, int theTrackIndex, int theRenderGroup, TodTriangleGroup* theTriangleGroup)
{
	return DrawTrackAtTime(g, theTrackIndex, theRenderGroup, theTriangleGroup, nullptr);
}
#endif

//0x472E40
void Reanimation::DrawRenderGroup(Graphics* g, int theRenderGroup)
{
	if (mDead)
		return;

	TodTriangleGroup aTriangleGroup;
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	ReanimatorFrameTime aFrameTime;
	bool aFrameTimeReady = false;
#endif
	for (int aTrackIndex = 0; aTrackIndex < mDefinition->mTracks.count; aTrackIndex++)
	{
		ReanimatorTrackInstance* aTrackInstance = &mTrackInstances[aTrackIndex];
		if (aTrackInstance->mRenderGroup == theRenderGroup)
		{
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
			if (!aFrameTimeReady)
			{
				GetFrameTime(&aFrameTime);
				aFrameTimeReady = true;
			}
			bool aTrackDrawn = DrawTrackAtTime(g, aTrackIndex, theRenderGroup, &aTriangleGroup, &aFrameTime);
#else
			bool aTrackDrawn = DrawTrack(g, aTrackIndex, theRenderGroup, &aTriangleGroup);
#endif
			if (aTrackInstance->mAttachmentID != AttachmentID::ATTACHMENTID_NULL)
			{
				aTriangleGroup.DrawGroup(g);
				AttachmentDraw(aTrackInstance->mAttachmentID, g, !aTrackDrawn);
			}
		}
	}
	aTriangleGroup.DrawGroup(g);
}

void Reanimation::Draw(Graphics* g)
{ 
	DrawRenderGroup(g, RENDER_GROUP_NORMAL);
}

//0x472F30
// GOTY @Patoke: 0x477640
int Reanimation::FindTrackIndex(const char* theTrackName)
{
	for (int aTrackIndex = 0; aTrackIndex < mDefinition->mTracks.count; aTrackIndex++)
		if (strcasecmp(mDefinition->mTracks.tracks[aTrackIndex].mName, theTrackName) == 0)
			return aTrackIndex;

	TodTrace("Can't find track '%s'", theTrackName);
	return 0;
}

// GOTY @Patoke: 0x464B18
ReanimatorTrackInstance* Reanimation::GetTrackInstanceByName(const char* theTrackName)
{
	return &mTrackInstances[FindTrackIndex(theTrackName)];
}

//0x472F80
void Reanimation::AttachToAnotherReanimation(Reanimation* theAttachReanim, const char* theTrackName)
{
	if (theAttachReanim->mDefinition->mTracks.count <= 0)
		return;

	if (theAttachReanim->mFrameBasePose == -1)
		theAttachReanim->mFrameBasePose = theAttachReanim->mFrameStart;  // 将当前动作的起始帧作为变换基准帧
	AttachReanim(theAttachReanim->GetTrackInstanceByName(theTrackName)->mAttachmentID, this, 0.0f, 0.0f);
}

void Reanimation::SetBasePoseFromAnim(const char* theTrackName)
{
	int aFrameStart, aFrameCount;
	GetFramesForLayer(theTrackName, aFrameStart, aFrameCount);
	mFrameBasePose = aFrameStart;  // 将当前轨道动画的起始帧作为变换基准帧
}

//0x472FD0
void Reanimation::GetTrackBasePoseMatrix(int theTrackIndex, SexyTransform2D& theBasePosMatrix)
{
	if (mFrameBasePose == NO_BASE_POSE)
	{
		theBasePosMatrix.LoadIdentity();
		return;
	}

	int aBasePos = mFrameBasePose == -1 ? mFrameStart : mFrameBasePose;
	ReanimatorFrameTime aStartTime = { 0.0f, aBasePos, aBasePos + 1 };
	ReanimatorTransform aTransformStart;
	GetTransformAtTime(theTrackIndex, &aTransformStart, &aStartTime);
	MatrixFromTransform(aTransformStart, theBasePosMatrix);
}

//0x473070
AttachEffect* Reanimation::AttachParticleToTrack(const char* theTrackName, TodParticleSystem* theParticleSystem, float thePosX, float thePosY)
{
	int aTrackIndex = FindTrackIndex(theTrackName);
	ReanimatorTrackInstance* aTrackInstance = &mTrackInstances[aTrackIndex];
	SexyTransform2D aBasePoseMatrix;
	GetTrackBasePoseMatrix(aTrackIndex, aBasePoseMatrix);  // 取得轨道基础形态的变换矩阵
	SexyVector2 aPosition = aBasePoseMatrix * SexyVector2(thePosX, thePosY);  // 以基础形态的矩阵变换位置向量
	return AttachParticle(aTrackInstance->mAttachmentID, theParticleSystem, aPosition.x, aPosition.y);
}

//0x473110
// GOTY @Patoke: 0x477810
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
void Reanimation::GetAttachmentOverlayMatrix(int theTrackIndex, SexyTransform2D& theOverlayMatrix, const ReanimatorTransform* theTransformOverride)
#else
void Reanimation::GetAttachmentOverlayMatrix(int theTrackIndex, SexyTransform2D& theOverlayMatrix)
#endif
{
	ReanimatorTransform aTransformStorage;
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	const ReanimatorTransform* aTransform = theTransformOverride;
	if (aTransform == nullptr)
	{
		GetCurrentTransform(theTrackIndex, &aTransformStorage);
		aTransform = &aTransformStorage;
	}
#else
	GetCurrentTransform(theTrackIndex, &aTransformStorage);  // 取得含混合、不含覆写的自然变换
	const ReanimatorTransform* aTransform = &aTransformStorage;
#endif

	SexyTransform2D aTransformMatrix;
	MatrixFromTransform(*aTransform, aTransformMatrix);
	SexyMatrix3Multiply(aTransformMatrix, mOverlayMatrix, aTransformMatrix);  // 以动画覆写矩阵作用于动画变换矩阵

	SexyTransform2D aBasePoseMatrixInv;
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	ReanimatorGetBasePoseInverseCached(this, theTrackIndex, aBasePoseMatrixInv);
#else
	SexyTransform2D aBasePoseMatrix;
	GetTrackBasePoseMatrix(theTrackIndex, aBasePoseMatrix);  // 取得轨道基础形态的变换矩阵
	SexyMatrix3Inverse(aBasePoseMatrix, aBasePoseMatrixInv);  // 取得基础形态矩阵的逆
#endif
	theOverlayMatrix = aTransformMatrix * aBasePoseMatrixInv;
}

//0x4731D0
void Reanimation::GetFramesForLayer(const char* theTrackName, int& theFrameStart, int& theFrameCount)
{
	if (mDefinition->mTracks.count == 0)  // 如果动画没有轨道
	{
		theFrameStart = 0;
		theFrameCount = 0;
		return;
	}

	int aTrackIndex = FindTrackIndex(theTrackName);
	TOD_ASSERT(aTrackIndex >= 0 && aTrackIndex < mDefinition->mTracks.count);
	ReanimatorTrack* aTrack = &mDefinition->mTracks.tracks[aTrackIndex];
	theFrameStart = 0;
	theFrameCount = 1;
	for (int i = 0; i < aTrack->mTransforms.count; i++)
		if (aTrack->mTransforms.mTransforms[i].mFrame >= 0.0f)
		{
			theFrameStart = i;  // 取轨道上的首个非空白帧作为起始帧
			break;
		}
	for (int j = theFrameStart; j < aTrack->mTransforms.count; j++)
		if (aTrack->mTransforms.mTransforms[j].mFrame >= 0.0f)
			theFrameCount = j - theFrameStart + 1;  // 取从起始帧至轨道最后一个非空白帧之间为帧数量
}

//0x473280
void Reanimation::SetFramesForLayer(const char* theTrackName)
{
	if (mAnimRate >= 0)
		mAnimTime = 0.0f;
	else
		mAnimTime = 0.9999999f;
	mLastFrameTime = -1.0f;
	GetFramesForLayer(theTrackName, mFrameStart, mFrameCount);
}

//0x4732C0
bool Reanimation::TrackExists(const char* theTrackName)
{
	for (int aTrackIndex = 0; aTrackIndex < mDefinition->mTracks.count; aTrackIndex++)
		if (strcasecmp(mDefinition->mTracks.tracks[aTrackIndex].mName, theTrackName) == 0)
			return true;
	return false;
}

//0x473310
void Reanimation::StartBlend(int theBlendTime)
{
	for (int aTrackIndex = 0; aTrackIndex < mDefinition->mTracks.count; aTrackIndex++)
	{
		ReanimatorTransform aTransform;
		GetCurrentTransform(aTrackIndex, &aTransform);
		if (FloatRoundToInt(aTransform.mFrame) >= 0)  // 若当前轨道当前不处于空白帧
		{
			ReanimatorTrackInstance* aTrackInstance = &mTrackInstances[aTrackIndex];
			aTrackInstance->mBlendTransform = aTransform;  // 记录当前变换为混合的初始（源）变换
			aTrackInstance->mBlendTime = theBlendTime;
			aTrackInstance->mBlendCounter = theBlendTime;
			aTrackInstance->mBlendTransform.mFont = nullptr;
			aTrackInstance->mBlendTransform.mText = "";
			aTrackInstance->mBlendTransform.mImage = nullptr;
		}
	}
}

//0x4733F0
void Reanimation::ReanimationDie()
{
	if (!mDead)
	{
		mDead = true;
		if (mDefinition == nullptr || mTrackInstances == nullptr)
			return;
		for (int aTrackIndex = 0; aTrackIndex < mDefinition->mTracks.count; aTrackIndex++)
		{
			TOD_ASSERT(mTrackInstances);
			AttachmentDie(mTrackInstances[aTrackIndex].mAttachmentID);
		}
	}
}

void Reanimation::SetShakeOverride(const char* theTrackName, float theShakeAmount)
{ 
	GetTrackInstanceByName(theTrackName)->mShakeOverride = theShakeAmount;
}

void Reanimation::SetPosition(float theX, float theY) 
{ 
	mOverlayMatrix.m02 = theX;
	mOverlayMatrix.m12 = theY;
}

void Reanimation::OverrideScale(float theScaleX, float theScaleY)
{
	mOverlayMatrix.m00 = theScaleX;
	mOverlayMatrix.m11 = theScaleY;
}

//0x473470
Image* Reanimation::GetImageOverride(const char* theTrackName)
{
	return GetTrackInstanceByName(theTrackName)->mImageOverride;
}

//0x473490
// GOTY @Patoke: 0x477BB0
void Reanimation::SetImageOverride(const char* theTrackName, Image* theImage)
{
	GetTrackInstanceByName(theTrackName)->mImageOverride = theImage;
}

//0x4734B0
void Reanimation::SetTruncateDisappearingFrames(const char* theTrackName, bool theTruncateDisappearingFrames)
{
	if (theTrackName == nullptr)  // 若给出的轨道名称为空指针
	{
		for (int aTrackIndex = 0; aTrackIndex < mDefinition->mTracks.count; aTrackIndex++)  // 依次设置每一轨道
			mTrackInstances[aTrackIndex].mTruncateDisappearingFrames = theTruncateDisappearingFrames;
	}
	else
		GetTrackInstanceByName(theTrackName)->mTruncateDisappearingFrames = theTruncateDisappearingFrames;
}

void ReanimationHolder::DisposeHolder()
{
	mReanimations.DataArrayDispose();
}

//0x473500
ReanimationHolder::~ReanimationHolder()
{
	DisposeHolder();
}

void ReanimationHolder::InitializeHolder()
{
	// 2048, not 1024: AllocReanimation is UNGUARDED — when the pool fills it hits
	// TOD_ASSERT(mSize != mMaxSize), and a failed assert runs exit(0) (see
	// TodAssertFailed), so the game just closes ("crash after a heavy wave"). A
	// dense horde of accessorized zombies blows past 1024: every zombie body,
	// every accessory (cone/bucket/door/flag = an Attachment carrying its own
	// reanim), every plant, animated projectile and death effect is one
	// reanimation. Particles/emitters degrade gracefully (they null-check the
	// pool); reanimations cannot (callers deref the result), so grow the pool.
	// The DataArray item is small (~150 B); the live cost is per-reanim track
	// instances allocated on demand, unchanged by the cap.
	mReanimations.DataArrayInitialize(2048U, "reanims");
}

//0x473590
Reanimation* ReanimationHolder::AllocReanimation(float theX, float theY, int theRenderOrder, ReanimationType theReanimationType)
{
	TOD_ASSERT(mReanimations.mSize != mReanimations.mMaxSize);
	Reanimation* aReanim = mReanimations.DataArrayAlloc();
	aReanim->mRenderOrder = theRenderOrder;
	aReanim->mReanimationHolder = this;
	aReanim->ReanimationInitializeType(theX, theY, theReanimationType);
	return aReanim;
}

//0x4735E0
void ReanimatorEnsureDefinitionLoaded(ReanimationType theReanimType, bool theIsPreloading)
{
	TOD_ASSERT(theReanimType >= 0 && theReanimType < gReanimatorDefCount);
	ReanimatorDefinition* aReanimDef = &gReanimatorDefArray[(int)theReanimType];
	if (aReanimDef->mTracks.tracks != nullptr)  // 如果轨道指针不为空指针，说明定义数据已经加载
		return;
	// Real load ahead (file + PNGs). Park the mixer so its audsrv SIF RPCs can't
	// collide with these reads on real hardware. Placed AFTER the already-loaded
	// early-out so the common no-op case never parks. Nests cheaply under the
	// loading thread's park and under CutScene::PreloadResources.
	PS2_PAUSE_MIXER_FOR_IO();
	ReanimationParams* aReanimParams = &gReanimationParamArray[(int)theReanimType];
	TodTrace("'%s'\n", aReanimParams->mReanimFileName);
	if (theIsPreloading)
	{
		if (gSexyAppBase->mShutdown || gAppCloseRequest())  // 预加载时若程序退出，则取消加载
			return;
	}
	else  // < 以下部分仅内测版执行 >
	{
		if (gAppHasUsedCheatKeys())
			TodTraceAndLog("Cheater failed to preload '%s' on %s", aReanimParams->mReanimFileName, gGetCurrentLevelName().c_str());
		else
			TodTraceAndLog("Non-cheater failed to preload '%s' on %s", aReanimParams->mReanimFileName, gGetCurrentLevelName().c_str());
	}  // < 以上部分仅内测版执行 >

	PerfTimer aTimer;
	aTimer.Start();
	TodHesitationBracket aHesitation("Load Reanim '%s'", aReanimParams->mReanimFileName);
	if (!ReanimationLoadDefinition(aReanimParams->mReanimFileName, aReanimDef))
	{
		char aBuf[1024];
		sprintf(aBuf, "Failed to load reanim '%s'", aReanimParams->mReanimFileName);
		TodErrorMessageBox(aBuf, "Error");
	}
	int aDuration = aTimer.GetDuration();
	if (aDuration > 100)  //（仅内测版）创建时间过长的报告
		TodTraceAndLog("LOADING:Long reanim '%s' %d ms on %s", aReanimParams->mReanimFileName, aDuration, gGetCurrentLevelName().c_str());
}

//0x473750
void ReanimatorLoadDefinitions(ReanimationParams* theReanimationParamArray, int theReanimationParamArraySize)
{
	TodHesitationBracket aHesitation(__S("ReanimatorLoadDefinitions"));
	TOD_ASSERT(!gReanimationParamArray && !gReanimatorDefArray);
	gReanimationParamArraySize = theReanimationParamArraySize;
	gReanimationParamArray = theReanimationParamArray;
	gReanimatorDefCount = theReanimationParamArraySize;
	gReanimatorDefArray = new ReanimatorDefinition[theReanimationParamArraySize];

#ifndef LOW_MEMORY
	for (unsigned int i = 0; i < gReanimationParamArraySize; i++)
	{
		ReanimationParams* aReanimationParams = &theReanimationParamArray[i];
		TOD_ASSERT(aReanimationParams->mReanimationType == i);
		if (DefinitionIsCompiled(StringToSexyString(aReanimationParams->mReanimFileName)))
			ReanimatorEnsureDefinitionLoaded(aReanimationParams->mReanimationType, true);
	}
#endif
}

//0x473870
void ReanimatorFreeDefinitions()
{
	for (unsigned int i = 0; i < gReanimatorDefCount; i++)
		ReanimationFreeDefinition(&gReanimatorDefArray[i]);

	delete[] gReanimatorDefArray;
	gReanimatorDefArray = nullptr;
	gReanimatorDefCount = 0;
	gReanimationParamArray = nullptr;
	gReanimationParamArraySize = 0;
}

// Collects the source images a loaded definition depends on: the originals
// packed into its atlas, plus raw Image* still referenced by transforms
// (images over 254px and NO_ATLAS reanims are drawn from the original).
// Atlas-encoded transforms store small indices instead of pointers (see
// ReanimAtlas::GetEncodedReanimAtlas), so anything <= 1000 is not a pointer.
static void ReanimationCollectSourceImages(ReanimatorDefinition* theDefinition, std::set<Image*>& theImages)
{
	ReanimAtlas* aAtlas = theDefinition->mReanimAtlas;
	if (aAtlas != nullptr)
	{
		for (int i = 0; i < aAtlas->mImageCount; i++)
		{
			if (aAtlas->mImageArray[i].mOriginalImage != nullptr)
				theImages.insert(aAtlas->mImageArray[i].mOriginalImage);
		}
	}

	for (int aTrackIndex = 0; aTrackIndex < theDefinition->mTracks.count; aTrackIndex++)
	{
		ReanimatorTrack* aTrack = &theDefinition->mTracks.tracks[aTrackIndex];
		for (int i = 0; i < aTrack->mTransforms.count; i++)
		{
			Image* aImage = aTrack->mTransforms.mTransforms[i].mImage;
			if (aImage != nullptr && (uintptr_t)aImage > 1000)
				theImages.insert(aImage);
		}
	}
}

// Frees every definition (tracks + atlas MemoryImage, which also releases its
// GS texture/VRAM) that no allocated Reanimation is using, then drops the
// lazily-loaded source images that no remaining definition references.
// Definitions reload transparently on the next ReanimatorEnsureDefinitionLoaded,
// so this is safe to call whenever the reanimation holder is quiescent —
// right after EffectSystemFreeAll at board creation/teardown.
// theOnlyTypes narrows the candidates: when non-null, only flagged types are
// considered (still subject to the in-use check), so callers can free a
// specific subset (e.g. seed-chooser plants) without touching level assets.
void ReanimatorFreeUnusedDefinitions(const std::vector<bool>* theOnlyTypes)
{
	if (gReanimatorDefArray == nullptr)
		return;

	std::vector<bool> aInUse(gReanimatorDefCount, false);
	if (gEffectSystem != nullptr && gEffectSystem->mReanimationHolder != nullptr)
	{
		Reanimation* aReanim = nullptr;
		while (gEffectSystem->mReanimationHolder->mReanimations.IterateNext(aReanim))
		{
			if (aReanim->mReanimationType >= 0 && (unsigned int)aReanim->mReanimationType < gReanimatorDefCount)
				aInUse[(int)aReanim->mReanimationType] = true;
		}
	}

	// The selector-screen definition is reloaded the moment the player returns
	// to the menu, and it is the biggest single def block (~1.9MB contiguous).
	// It loads at boot into a compact heap; freeing it on board teardown means
	// re-allocating that block into whatever the level just fragmented —
	// observed 2026-07-18: bad_alloc for 1900725 bytes with 10MB free.
	// Keep it resident so its block stays at its boot-time address for good.
	if ((unsigned int)ReanimationType::REANIM_SELECTOR_SCREEN < gReanimatorDefCount)
		aInUse[(int)ReanimationType::REANIM_SELECTOR_SCREEN] = true;

	int aFreedCount = 0;
	std::set<Image*> aFreedImages;
	for (unsigned int i = 0; i < gReanimatorDefCount; i++)
	{
		ReanimatorDefinition* aDef = &gReanimatorDefArray[i];
		if (aDef->mTracks.tracks == nullptr || aInUse[i])
			continue;
		if (theOnlyTypes != nullptr && (i >= theOnlyTypes->size() || !(*theOnlyTypes)[i]))
			continue;

		ReanimationCollectSourceImages(aDef, aFreedImages);
		ReanimationFreeDefinition(aDef);
		aFreedCount++;
	}
	if (aFreedCount == 0)
		return;

	// A source image may be shared between a freed and a still-loaded
	// definition (e.g. common zombie parts), so only delete the orphans.
	std::set<Image*> aStillUsed;
	for (unsigned int i = 0; i < gReanimatorDefCount; i++)
	{
		if (gReanimatorDefArray[i].mTracks.tracks != nullptr)
			ReanimationCollectSourceImages(&gReanimatorDefArray[i], aStillUsed);
	}
	for (Image* aImage : aStillUsed)
		aFreedImages.erase(aImage);

#ifdef SEXY_LAZY_IMAGES
	// Keep the Image object shells, but evict the payload/texture of source
	// images that became orphaned when their last reanimation definition was
	// freed.  Keeping the shell is important: particle definitions and other
	// long-lived data can retain the Image* even though no loaded reanimation
	// definition references it.  PurgeLazyImage(..., false) is safe for those
	// stale pointers because a future draw faults the same shell back in.
	//
	// This is also what makes mid-level collection useful on LOW_MEMORY
	// consoles.  Previously ReanimationFreeDefinition released its atlas, but
	// every lazily loaded source texture stayed resident until a screen-level
	// PurgeLazyImages().  FINAL_BOSS continuously introduces zombie types with
	// no screen transition, so the Wii graphics arena eventually filled even
	// though the zombies using many of those definitions were already dead.
	int aPurgedSources = 0;
	for (Image* aImage : aFreedImages)
	{
		MemoryImage* aMemoryImage = static_cast<MemoryImage*>(aImage);
		if (gSexyAppBase->PurgeLazyImage(aMemoryImage, false))
			aPurgedSources++;
	}

	// Keep the Image object shells: deleting the objects only recovers ~200B
	// each while being the one step here that can leave a dangling Image*.
	// Suspected in the 2026-07-18 TLB crash inside TextureData::CreateTextures,
	// where a pointer was stomped with "…ean…" string bytes (freed block
	// reused by a path string).
	//
	// The condition is SEXY_LAZY_IMAGES and not PS2_PLATFORM, which is what it
	// used to be. Every word of the reasoning above is about the lazy machinery
	// reclaiming the payload, not about the PS2 — so the moment Wii joined
	// SEXY_LAZY_IMAGES (MemoryImage.h) it needed this too, and instead it kept
	// taking the branch below. It produced the same crash in the same function:
	// Wii, 2026-08-08, ISI with PC == CTR == 0xDC800000 inside
	// TextureData::CreateTextures, reached from a particle's TodTriangleGroup —
	// a jump through the vtable of a deleted MemoryImage, since CreateTextures
	// opens on the virtual DeleteSWBuffers().
	//
	// The orphan test above is the reason a dangling pointer is reachable at
	// all: it asks which images the remaining *reanimation* definitions still
	// reference, and nothing else. A TodEmitterDefinition holding the same
	// Image* in mImage is invisible to it, so a particle emitter can outlive
	// the image it draws with.
	TodTrace("freed %d reanim defs, purged %d/%d orphan source images (shells kept)",
		aFreedCount, aPurgedSources, (int)aFreedImages.size());
#else
	int aDeletedImages = ((TodResourceManager*)gSexyAppBase->mResourceManager)->DeleteUngroupedImages(aFreedImages);
	gSexyAppBase->CleanSharedImages();
	TodTrace("freed %d reanim defs, %d source images", aFreedCount, aDeletedImages);
#endif
}

// Frees a single definition if no live Reanimation uses it. Used on LOW_MEMORY
// targets to cap the RAM peak while the seed chooser walks every unlocked
// plant building its cached packet frame.
void ReanimatorFreeUnusedDefinition(ReanimationType theReanimType)
{
	if (gReanimatorDefArray == nullptr || theReanimType < 0 || (unsigned int)theReanimType >= gReanimatorDefCount)
		return;

	std::vector<bool> aOnlyThis(gReanimatorDefCount, false);
	aOnlyThis[(int)theReanimType] = true;
	ReanimatorFreeUnusedDefinitions(&aOnlyThis);
}

//0x4738D0
float Reanimation::GetTrackVelocity(const char* theTrackName)
{
	ReanimatorFrameTime aFrameTime;
	GetFrameTime(&aFrameTime);
	int aTrackIndex = FindTrackIndex(theTrackName);
	TOD_ASSERT(aTrackIndex >= 0 && aTrackIndex < mDefinition->mTracks.count);

	ReanimatorTrack* aTrack = &mDefinition->mTracks.tracks[aTrackIndex];
	float aDis = aTrack->mTransforms.mTransforms[aFrameTime.mAnimFrameAfterInt].mTransX - aTrack->mTransforms.mTransforms[aFrameTime.mAnimFrameBeforeInt].mTransX;
	return aDis * SECONDS_PER_UPDATE * mAnimRate;  // 瞬时速率 = 两帧间的横坐标之差 * 一帧的时长 * 动画速率
}

//0x473930
bool Reanimation::IsTrackShowing(const char* theTrackName)
{
	ReanimatorFrameTime aFrameTime;
	GetFrameTime(&aFrameTime);
	int aTrackIndex = FindTrackIndex(theTrackName);
	TOD_ASSERT(aTrackIndex >= 0 && aTrackIndex < mDefinition->mTracks.count);

	return mDefinition->mTracks.tracks[aTrackIndex].mTransforms.mTransforms[aFrameTime.mAnimFrameAfterInt].mFrame >= 0.0f;  // 返回下一整数帧是否存在图像
}

//0x473980
void Reanimation::ShowOnlyTrack(const char* theTrackName)
{
	for (int i = 0; i < mDefinition->mTracks.count; i++)
	{
		// 轨道名与指定名称相同时，设置轨道渲染分组为正常显示，否则设置轨道渲染分组为隐藏
		mTrackInstances[i].mRenderGroup = strcasecmp(mDefinition->mTracks.tracks[i].mName, theTrackName) == 0 ? RENDER_GROUP_NORMAL : RENDER_GROUP_HIDDEN;
	}
}

//0x4739E0
// GOTY @Patoke: 0x478120
void Reanimation::AssignRenderGroupToTrack(const char* theTrackName, int theRenderGroup)
{
	for (int i = 0; i < mDefinition->mTracks.count; i++)
		if (strcasecmp(mDefinition->mTracks.tracks[i].mName, theTrackName) == 0)
		{
			mTrackInstances[i].mRenderGroup = theRenderGroup;  // 仅设置首个名称恰好为 theTrackName 的轨道
			return;
		}
}

//0x473A40
// GOTY @Patoke: 0x478170
void Reanimation::AssignRenderGroupToPrefix(const char* theTrackName, int theRenderGroup)
{
	size_t aPrifixLength = strlen(theTrackName);
	for (int i = 0; i < mDefinition->mTracks.count; i++)
	{
		const char* const aTrackName = mDefinition->mTracks.tracks[i].mName;
		if (strlen(aTrackName) >= aPrifixLength && !strncasecmp(aTrackName, theTrackName, aPrifixLength))  // 轨道名称长度必须不小于指定前缀长度
			mTrackInstances[i].mRenderGroup = theRenderGroup;
	}
}

//0x473AE0
void Reanimation::PropogateColorToAttachments()
{
	for (int i = 0; i < mDefinition->mTracks.count; i++)
		AttachmentPropogateColor(
			mTrackInstances[i].mAttachmentID, mColorOverride, mEnableExtraAdditiveDraw, mExtraAdditiveColor, mEnableExtraOverlayDraw, mExtraOverlayColor
		);
}

//0x473B70
bool Reanimation::ShouldTriggerTimedEvent(float theEventTime)
{
	TOD_ASSERT(theEventTime >= 0.0f && theEventTime <= 1.0f);
	if (mFrameCount == 0 || mLastFrameTime <= 0.0f || mAnimRate <= 0.0f)  // 没有动画或倒放或未播放
		return false;

	if (mAnimTime >= mLastFrameTime)  // 一般情况下，可触发的范围为 [mLastFrameTime, mAnimTime]
		return theEventTime >= mLastFrameTime && theEventTime < mAnimTime;
	else  // 若动画正好完成一次循环而重新进入下一次循环，则可触发的范围为 [0, mAnimTime] ∪ [mLastFrameTime, 1]
		return theEventTime >= mLastFrameTime || theEventTime < mAnimTime;
}
//0x473BF0
// GOTY @Patoke: 0x478310
void Reanimation::PlayReanim(const char* theTrackName, ReanimLoopType theLoopType, int theBlendTime, float theAnimRate)
{
	if (theBlendTime > 0)  // 当需要补间过渡时，开始混合
		StartBlend(theBlendTime);
	if (theAnimRate != 0.0f)  // 当指定的速率为 0 时，表示不改变原有动画速率
		mAnimRate = theAnimRate;

	mLoopType = theLoopType;
	mLoopCount = 0;
	SetFramesForLayer(theTrackName);
}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
static bool ReanimatorSpanEquals(const char* theText, unsigned short theTextLength, const char* theExpected)
{
	size_t aExpectedLength = strlen(theExpected);
	return aExpectedLength == theTextLength && memcmp(theText, theExpected, theTextLength) == 0;
}

static bool ReanimatorSpanEqualsNoCase(const char* theText, unsigned short theTextLength, const char* theExpected)
{
	size_t aExpectedLength = strlen(theExpected);
	return aExpectedLength == theTextLength && strncasecmp(theText, theExpected, theTextLength) == 0;
}

static ReanimationType ReanimatorFindTypeForName(const char* theName, size_t theNameLength)
{
	static const char aPrefix[] = "reanim/";
	static const char aSuffix[] = ".reanim";
	const size_t aPrefixLength = sizeof(aPrefix) - 1;
	const size_t aSuffixLength = sizeof(aSuffix) - 1;

	for (unsigned int i = 0; i < gReanimationParamArraySize; i++)
	{
		const char* aFileName = gReanimationParamArray[i].mReanimFileName;
		if (aFileName == nullptr)
			continue;

		size_t aFileNameLength = strlen(aFileName);
		if (aFileNameLength != aPrefixLength + theNameLength + aSuffixLength)
			continue;
		if (strncasecmp(aFileName, aPrefix, aPrefixLength) != 0)
			continue;
		if (strncasecmp(aFileName + aPrefixLength, theName, theNameLength) != 0)
			continue;
		if (strcasecmp(aFileName + aPrefixLength + theNameLength, aSuffix) == 0)
			return gReanimationParamArray[i].mReanimationType;
	}

	return ReanimationType::REANIM_NONE;
}

static void ReanimatorParseAttacherCached(const ReanimatorTransform& theTransform, ReanimatorAttacherCache& theCache)
{
	theCache.mSourceText = theTransform.mText;
	theCache.mTrackName = nullptr;
	theCache.mTrackNameLength = 0;
	theCache.mReanimationType = ReanimationType::REANIM_NONE;
	theCache.mAnimRate = 12.0f;
	theCache.mLoopType = (unsigned char)ReanimLoopType::REANIM_LOOP;
	theCache.mValid = 1;

	if (theTransform.mText == nullptr)
		return;

	const char* aReanimName = strstr(theTransform.mText, "__");
	if (aReanimName == nullptr)
		return;

	const char* aNameStart = aReanimName + 2;
	const char* aTags = strstr(aNameStart, "[");
	const char* aTrackMarker = strstr(aNameStart, "__");
	if (aTags != nullptr && aTrackMarker != nullptr && aTags < aTrackMarker)
		return;

	const char* aNameEnd = aTrackMarker != nullptr ? aTrackMarker : (aTags != nullptr ? aTags : aNameStart + strlen(aNameStart));
	size_t aNameLength = (size_t)(aNameEnd - aNameStart);
	if (aNameLength != 0)
		theCache.mReanimationType = ReanimatorFindTypeForName(aNameStart, aNameLength);

	if (aTrackMarker != nullptr)
	{
		const char* aTrackStart = aTrackMarker + 2;
		const char* aTrackEnd = aTags != nullptr ? aTags : aTrackStart + strlen(aTrackStart);
		size_t aTrackLength = (size_t)(aTrackEnd - aTrackStart);
		if (aTrackLength <= 0xFFFFU)
		{
			theCache.mTrackName = aTrackStart;
			theCache.mTrackNameLength = (unsigned short)aTrackLength;
		}
	}

	while (aTags != nullptr)
	{
		const char* aTagEnd = strstr(aTags + 1, "]");
		if (aTagEnd == nullptr)
			break;

		char* aNumberEnd = nullptr;
		float aAnimRate = strtof(aTags + 1, &aNumberEnd);
		if (aNumberEnd != aTags + 1)
		{
			theCache.mAnimRate = aAnimRate;
		}
		else
		{
			size_t aTagLength = (size_t)(aTagEnd - aTags - 1);
			if (aTagLength == 4 && memcmp(aTags + 1, "hold", 4) == 0)
				theCache.mLoopType = (unsigned char)ReanimLoopType::REANIM_PLAY_ONCE_AND_HOLD;
			else if (aTagLength == 4 && memcmp(aTags + 1, "once", 4) == 0)
				theCache.mLoopType = (unsigned char)ReanimLoopType::REANIM_PLAY_ONCE;
		}

		aTags = strstr(aTagEnd + 1, "[");
	}
}

static ReanimatorAttacherCache& ReanimatorGetAttacherCached(const ReanimatorTransform& theTransform)
{
	uintptr_t aTextKey = reinterpret_cast<uintptr_t>(theTransform.mText);
	unsigned int aCacheIndex = (unsigned int)((aTextKey >> 4U) & (REANIMATOR_ATTACHER_CACHE_SIZE - 1U));
	for (unsigned int aProbe = 0; aProbe < 4U; aProbe++)
	{
		ReanimatorAttacherCache& aCache = gReanimatorAttacherCache[(aCacheIndex + aProbe) & (REANIMATOR_ATTACHER_CACHE_SIZE - 1U)];
		if (aCache.mValid && aCache.mSourceText == theTransform.mText)
			return aCache;
		if (!aCache.mValid)
		{
			ReanimatorParseAttacherCached(theTransform, aCache);
			return aCache;
		}
	}

	ReanimatorAttacherCache& aCache = gReanimatorAttacherCache[aCacheIndex];
	ReanimatorParseAttacherCached(theTransform, aCache);
	return aCache;
}

static int ReanimatorFindTrackIndexSpan(Reanimation* theReanimation, const char* theTrackName, unsigned short theTrackNameLength)
{
	for (int aTrackIndex = 0; aTrackIndex < theReanimation->mDefinition->mTracks.count; aTrackIndex++)
	{
		const char* aTrackName = theReanimation->mDefinition->mTracks.tracks[aTrackIndex].mName;
		if (ReanimatorSpanEqualsNoCase(theTrackName, theTrackNameLength, aTrackName))
			return aTrackIndex;
	}

	TodTrace("Can't find track '%.*s'", (int)theTrackNameLength, theTrackName);
	return 0;
}

static void ReanimatorGetFramesForLayerSpan(Reanimation* theReanimation, const char* theTrackName, unsigned short theTrackNameLength, int& theFrameStart, int& theFrameCount)
{
	if (theReanimation->mDefinition->mTracks.count == 0)
	{
		theFrameStart = 0;
		theFrameCount = 0;
		return;
	}

	int aTrackIndex = ReanimatorFindTrackIndexSpan(theReanimation, theTrackName, theTrackNameLength);
	ReanimatorTrack* aTrack = &theReanimation->mDefinition->mTracks.tracks[aTrackIndex];
	theFrameStart = 0;
	theFrameCount = 1;
	for (int i = 0; i < aTrack->mTransforms.count; i++)
	{
		if (aTrack->mTransforms.mTransforms[i].mFrame >= 0.0f)
		{
			theFrameStart = i;
			break;
		}
	}
	for (int i = theFrameStart; i < aTrack->mTransforms.count; i++)
	{
		if (aTrack->mTransforms.mTransforms[i].mFrame >= 0.0f)
			theFrameCount = i - theFrameStart + 1;
	}
}

static void ReanimatorSetFramesForLayerSpan(Reanimation* theReanimation, const char* theTrackName, unsigned short theTrackNameLength)
{
	if (theReanimation->mAnimRate >= 0.0f)
		theReanimation->mAnimTime = 0.0f;
	else
		theReanimation->mAnimTime = 0.9999999f;
	theReanimation->mLastFrameTime = -1.0f;
	ReanimatorGetFramesForLayerSpan(theReanimation, theTrackName, theTrackNameLength, theReanimation->mFrameStart, theReanimation->mFrameCount);
}
#endif

//0x473C60
void Reanimation::ParseAttacherTrack(const ReanimatorTransform& theTransform, AttacherInfo& theAttacherInfo)
{
	theAttacherInfo.mReanimName = "";
	theAttacherInfo.mTrackName = "";
	theAttacherInfo.mAnimRate = 12.0f;
	theAttacherInfo.mLoopType = ReanimLoopType::REANIM_LOOP;
	if (theTransform.mFrame == -1.0f)  // 如果是空白帧
		return;

	/* 附属轨道名称格式：attacher__REANIMNAME__TRACKNAME[TAG1][TAG2]…… */

	const char* aReanimName = strstr(theTransform.mText, "__");  // 指向动画名称前的双下划线
	if (aReanimName == nullptr)  // 如果字符串中不含双下划线
		return;
	const char* aTags = strstr(aReanimName + 2, "[");  // 动画名称之后，指向 TAG 前的中括号
	const char* aTrackName = strstr(aReanimName + 2, "__");  // 动画名称之后，指向轨道名称前的双下划线
	if (aTags && aTrackName && ((uintptr_t)aTags < (uintptr_t)aTrackName))  // 如果“[”之后还有双下划线，则字符串非法
		return;

	if (aTrackName)  // 如果有定义轨道名称
	{
		theAttacherInfo.mReanimName.assign(aReanimName + 2, aTrackName - aReanimName - 2);  // 取两处双下划线之间的部分（REANIMNAME）
		if (aTags)  // 如果有定义标签
			theAttacherInfo.mTrackName.assign(aTrackName + 2, aTags - aTrackName - 2);  // 取到 TAG 的中括号之前
		else
			theAttacherInfo.mTrackName.assign(aTrackName + 2);  // 取到字符串结尾
	}
	else if (aTags)  // 如果未定义轨道名称但定义了标签
		theAttacherInfo.mReanimName.assign(aReanimName + 2, aTags - aReanimName - 2);  // 取双下划线至中括号之间的部分
	else  // 如果只定义了轨道名称
		theAttacherInfo.mReanimName.assign(aReanimName + 2);  // 从双下划线之后取到字符串结尾

	while (aTags)  // 读取每个 TAG
	{
		const char* aTagEnds = strstr(aTags + 1, "]");
		if (aTagEnds == nullptr)  // 如果没有右中括号
			break;
		
		std::string aCode(aTags + 1, aTagEnds - aTags - 1);  // 取中括号内的文本
		if (sscanf(aCode.c_str(), "%f", &theAttacherInfo.mAnimRate) != 1)  // 尝试将文本作为浮点数扫描，如果扫描成功则将结果作为动画速率
		{
			if (aCode.compare("hold") == 0)
				theAttacherInfo.mLoopType = ReanimLoopType::REANIM_PLAY_ONCE_AND_HOLD;
			else if (aCode.compare("once") == 0)
				theAttacherInfo.mLoopType = ReanimLoopType::REANIM_PLAY_ONCE;
		}

		aTags = strstr(aTagEnds + 1, "[");  // 继续寻找下一个 TAG 的左中括号
	}
}

//0x473EB0
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
void Reanimation::AttacherSynchWalkSpeed(int theTrackIndex, Reanimation* theAttachReanim)
#else
void Reanimation::AttacherSynchWalkSpeed(int theTrackIndex, Reanimation* theAttachReanim, AttacherInfo& theAttacherInfo)
#endif
{
#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM)
	(void)theAttacherInfo;
#endif
	ReanimatorTrack* aTrack = &mDefinition->mTracks.tracks[theTrackIndex];
	ReanimatorFrameTime aFrameTime;
	GetFrameTime(&aFrameTime);

	int aPlaceHolderFrameStart = aFrameTime.mAnimFrameBeforeInt;
	while (aPlaceHolderFrameStart > mFrameStart && aTrack->mTransforms.mTransforms[aPlaceHolderFrameStart - 1].mText == aTrack->mTransforms.mTransforms[aPlaceHolderFrameStart].mText)
		aPlaceHolderFrameStart--;  // 取当前所在区间的第一帧
	int aPlaceHolderFrameEnd = aFrameTime.mAnimFrameBeforeInt;
	while (aPlaceHolderFrameEnd < mFrameStart + mFrameCount - 1 && aTrack->mTransforms.mTransforms[aPlaceHolderFrameEnd + 1].mText == aTrack->mTransforms.mTransforms[aPlaceHolderFrameEnd].mText)
		aPlaceHolderFrameEnd++;  // 取当前所在区间的最后一帧
	int aPlaceHolderFrameCount = aPlaceHolderFrameEnd - aPlaceHolderFrameStart;
	ReanimatorTransform& aPlaceHolderStartTrans = aTrack->mTransforms.mTransforms[aPlaceHolderFrameStart];
	ReanimatorTransform& aPlaceHolderEndTrans = aTrack->mTransforms.mTransforms[aPlaceHolderFrameEnd];
	if (FloatApproxEqual(mAnimRate, 0.0f))  // 如果动画自身的速率为 0
	{
		theAttachReanim->mAnimRate = 0.0f;  // 附属动画的速率也为 0
		return;
	}
	float aPlaceHolderDistance = -(aPlaceHolderEndTrans.mTransX - aPlaceHolderStartTrans.mTransX);  // 占位轨道在当前区间内的位移
	float aPlaceHolderSeconds = aPlaceHolderFrameCount / mAnimRate;  // 占位轨道在当前区间内的时长
	if (FloatApproxEqual(aPlaceHolderSeconds, 0.0f))  // 如果当前所在区间不存在任何帧
	{
		theAttachReanim->mAnimRate = 0.0f;  // 附属动画的速率为 0
		return;
	}

	int aGroundTrackIndex = theAttachReanim->FindTrackIndex("_ground");
	ReanimatorTrack* aGroundTrack = &theAttachReanim->mDefinition->mTracks.tracks[aGroundTrackIndex];
	ReanimatorTransform& aTransformGuyStart = aGroundTrack->mTransforms.mTransforms[theAttachReanim->mFrameStart];
	ReanimatorTransform& aTransformGuyEnd = aGroundTrack->mTransforms.mTransforms[theAttachReanim->mFrameStart + theAttachReanim->mFrameCount - 1];
	float aGuyDistance = aTransformGuyEnd.mTransX - aTransformGuyStart.mTransX;  // 实际动画在完整动作周期内的位移
	if (aGuyDistance < FLT_EPSILON || aPlaceHolderDistance < FLT_EPSILON)  // 如果占位位移为 0 或实际动画周期位移为 0，则附属动画无法移动
	{
		theAttachReanim->mAnimRate = 0.0f;  // 附属动画的速率为 0
		return;
	}

	float aLoops = aPlaceHolderDistance / aGuyDistance;  // 以附属动画目标位移（占位位移）除以其周期位移，得到附属动画需要循环的周期数
	ReanimatorTransform aTransformGuyCurrent;
	theAttachReanim->GetCurrentTransform(aGroundTrackIndex, &aTransformGuyCurrent);
	AttachEffect* aAttachEffect = FindFirstAttachment(mTrackInstances[theTrackIndex].mAttachmentID);
	if (aAttachEffect != nullptr)
	{
		float aGuyCurrentDistance = aTransformGuyCurrent.mTransX - aTransformGuyStart.mTransX;  // 附属动画在其周期内当前已经过的位移
		float aGuyExpectedDistance = aGuyDistance * theAttachReanim->mAnimTime;  // 以匀速运动的占位轨道计算的、附属动画当前的理论位移
		aAttachEffect->mOffset.m02 = aGuyExpectedDistance - aGuyCurrentDistance;  // 调整附属效果的横向变换以使附属动画的位移保持与占位动画一致
	}
	theAttachReanim->mAnimRate = aLoops * theAttachReanim->mFrameCount / aPlaceHolderSeconds;  // 速率 = 需要播放的帧数 ÷ 可以播放的时长
}

//0x4740B0
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
void Reanimation::UpdateAttacherTrack(int theTrackIndex, const ReanimatorTransform* theTransformOverride)
#else
void Reanimation::UpdateAttacherTrack(int theTrackIndex)
#endif
{
	ReanimatorTrackInstance* aTrackInstance = &mTrackInstances[theTrackIndex];
	ReanimatorTransform aTransformStorage;
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	const ReanimatorTransform* aTransformPtr = theTransformOverride;
	if (aTransformPtr == nullptr)
	{
		GetCurrentTransform(theTrackIndex, &aTransformStorage);
		aTransformPtr = &aTransformStorage;
	}
	const ReanimatorTransform& aTransform = *aTransformPtr;
#else
	GetCurrentTransform(theTrackIndex, &aTransformStorage);
	const ReanimatorTransform& aTransform = aTransformStorage;
#endif

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	ReanimatorAttacherCache& aAttacherInfo = ReanimatorGetAttacherCached(aTransform);
	ReanimationType aReanimationType = aTransform.mFrame == -1.0f ? ReanimationType::REANIM_NONE : aAttacherInfo.mReanimationType;
#else
	AttacherInfo aAttacherInfo;
	ParseAttacherTrack(aTransform, aAttacherInfo);

	ReanimationType aReanimationType = ReanimationType::REANIM_NONE;
	if (aAttacherInfo.mReanimName.size() != 0)  // 如果附属轨道设定了当前的附属动画名称
	{
		std::string aReanimFileName = StrFormat("reanim/%s.reanim", aAttacherInfo.mReanimName.c_str());
		for (unsigned int i = 0; i < gReanimationParamArraySize; i++)  // 在动画参数数组中寻找动画文件名对应的动画类型
		{
			ReanimationParams* aParams = &gReanimationParamArray[i];
			if (strcasecmp(aReanimFileName.c_str(), aParams->mReanimFileName) == 0)
			{
				aReanimationType = aParams->mReanimationType;
				break;
			}
		}
	}
#endif

	if (aReanimationType == ReanimationType::REANIM_NONE)  // 如果没有设定当前附属动画名称，或未找到相应的动画
	{
		AttachmentDie(aTrackInstance->mAttachmentID);  // 清除附件
		return;
	}

	Reanimation* aAttachReanim = FindReanimAttachment(aTrackInstance->mAttachmentID);
	if (aAttachReanim == nullptr || aAttachReanim->mReanimationType != aReanimationType)  // 如果原先没有附属动画，或原附属动画不是上述设定的动画
	{
		AttachmentDie(aTrackInstance->mAttachmentID);  // 清除原有附件
		aAttachReanim = gEffectSystem->mReanimationHolder->AllocReanimation(0.0f, 0.0f, 0, aReanimationType);  // 重新创建一个指定的动画
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
		aAttachReanim->mLoopType = (ReanimLoopType)aAttacherInfo.mLoopType;
		aAttachReanim->mAnimRate = aAttacherInfo.mAnimRate;
#else
		aAttachReanim->mLoopType = aAttacherInfo.mLoopType;
		aAttachReanim->mAnimRate = aAttacherInfo.mAnimRate;
#endif
		AttachReanim(aTrackInstance->mAttachmentID, aAttachReanim, 0.0f, 0.0f);
		mFrameBasePose = NO_BASE_POSE;  // 设定附属动画后，自身不再存在基准帧
	}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	if (aAttacherInfo.mTrackNameLength != 0)  // 如果定义了附属动画的动作轨道
	{
		int aAnimFrameStart, aAnimFrameCount;
		ReanimatorGetFramesForLayerSpan(aAttachReanim, aAttacherInfo.mTrackName, aAttacherInfo.mTrackNameLength, aAnimFrameStart, aAnimFrameCount);
		if (aAttachReanim->mFrameStart != aAnimFrameStart || aAttachReanim->mFrameCount != aAnimFrameCount)
		{
			aAttachReanim->StartBlend(20);
			ReanimatorSetFramesForLayerSpan(aAttachReanim, aAttacherInfo.mTrackName, aAttacherInfo.mTrackNameLength);
		}

		if (aAttachReanim->mAnimRate == 12.0f && ReanimatorSpanEquals(aAttacherInfo.mTrackName, aAttacherInfo.mTrackNameLength, "anim_walk") && aAttachReanim->TrackExists("_ground"))
			AttacherSynchWalkSpeed(theTrackIndex, aAttachReanim);
		else
			aAttachReanim->mAnimRate = aAttacherInfo.mAnimRate;
		aAttachReanim->mLoopType = (ReanimLoopType)aAttacherInfo.mLoopType;
	}
#else
	if (aAttacherInfo.mTrackName.size() != 0)  // 如果定义了附属动画的动作轨道
	{
		int aAnimFrameStart, aAnimFrameCount;
		aAttachReanim->GetFramesForLayer(aAttacherInfo.mTrackName.c_str(), aAnimFrameStart, aAnimFrameCount);
		if (aAttachReanim->mFrameStart != aAnimFrameStart || aAttachReanim->mFrameCount != aAnimFrameCount)  // if (!aAttachReanim->IsAnimPlaying(……))
		{
			aAttachReanim->StartBlend(20);
			aAttachReanim->SetFramesForLayer(aAttacherInfo.mTrackName.c_str());  // 播放指定轨道上的动作
		}

		if (aAttachReanim->mAnimRate == 12.0f && aAttacherInfo.mTrackName.compare("anim_walk") == 0 && aAttachReanim->TrackExists("_ground"))
			AttacherSynchWalkSpeed(theTrackIndex, aAttachReanim, aAttacherInfo);
		else
			aAttachReanim->mAnimRate = aAttacherInfo.mAnimRate;
		aAttachReanim->mLoopType = aAttacherInfo.mLoopType;
	}
#endif

	Color aColor = ColorsMultiply(mColorOverride, aTrackInstance->mTrackColor);
	aColor.mAlpha = ClampInt(FloatRoundToInt(aTransform.mAlpha * aColor.mAlpha), 0, 255);
	AttachmentPropogateColor(aTrackInstance->mAttachmentID, aColor, mEnableExtraAdditiveDraw, mExtraAdditiveColor, mEnableExtraOverlayDraw, mExtraOverlayColor);
}

//0x4745B0
bool Reanimation::IsAnimPlaying(const char* theTrackName)
{
	int aFrameStart, aFrameCount;
	GetFramesForLayer(theTrackName, aFrameStart, aFrameCount);
	return mFrameStart == aFrameStart && mFrameCount == aFrameCount;
}

//0x4745F0
Reanimation* Reanimation::FindSubReanim(ReanimationType theReanimType)
{
	if (mReanimationType == theReanimType)
		return this;

	for (int i = 0; i < mDefinition->mTracks.count; i++)
	{
		Reanimation* aReanimation = FindReanimAttachment(mTrackInstances[i].mAttachmentID);
		if (aReanimation != nullptr)
		{
			Reanimation* aSubReanim = aReanimation->FindSubReanim(theReanimType);
			if (aSubReanim != nullptr)
				return aSubReanim;
		}
	}

	return nullptr;
}
