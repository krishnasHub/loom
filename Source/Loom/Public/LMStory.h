#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LMJson.h"
#include "LMStory.generated.h"

struct FLMQuest { FString Status = TEXT("none"); int32 Progress = 0; };   // none -> active -> complete -> turnedIn

/** What a dialogue UI shows for one choice. */
struct FLMChoiceView
{
	FString Text;
	FString VerbId;             // "" or the choice's verb ("persuade", "hack", ...)
	FString Verb;               // label shown before the text ("Persuade"); DecorateVerb may extend it ("Hack · 20 energy")
	bool bEnabled = true;
	FString Odds;               // only with bShowOdds (tuning)
};

/** The hidden-check formula: chance = Base + PerPoint x (stat - resolve) + verb bonus + disposition / DispositionDiv
 *  + matching bonuses, clamped to Min..Max (percent). Score = stat + secondary stat x SecondaryWeight. */
struct FLMCheckRules
{
	float Base = 50.f, PerPoint = 5.f, DispositionDiv = 4.f, SecondaryWeight = 0.5f, Min = 5.f, Max = 95.f;
	float DefaultResolve = 5.f;
	float DispositionMin = -100.f, DispositionMax = 100.f;
};

/** Who is being talked to. */
struct FLMSpeaker
{
	FString Name;
	FLinearColor Color = FLinearColor::White;
	FString Key;                // disposition and seeded-check key ("guard_captain", ...)
	FString Root;               // root dialogue node
};

DECLARE_MULTICAST_DELEGATE(FLMEvent);
DECLARE_MULTICAST_DELEGATE_TwoParams(FLMQuestEvent, const FString& /*QuestId*/, FName /*What: started, complete, turnedIn*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FLMOutcome, const FString& /*Encounter*/, const FString& /*Outcome*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FLMMoodEvent, float /*Mood*/, int32 /*Band*/);

/**
 * The story state of one world, and the dialogue engine that reads and changes it.
 *
 * Data (JSON, handed over with SetData): "dialogue" nodes, "dialogueVerbs", "quests", "encounters".
 *
 *   node     { "text": "..." | [ { "if": cond, "text": "..." }, ... ], "do": [ action... ], "choices": [ choice... ] }
 *   choice   { "text", "if": cond, "do": [ action... ], "next": node, "verb": id, "marker": "!" | "?",
 *              "check": { "resolve", "bonus": [ { "if", "add" } ], "success": node, "fail": node,
 *                         "level": { "min": n, "per": pct }, "tooLow": node } }
 *   cond     an object (all its keys must hold) or an array of them (all must hold). Built in:
 *              { "not": cond } { "quest": id, "is": status | [statuses] } { "flag": key [, "is": value] } { "disposition": { "gte": n } }
 *              { "mood": { "gte": n } | { "lte": n } }
 *   action   { "if": cond, <key>: value }. Built in: startQuest, turnIn, setFlag, disposition, resolve ("encounter:outcome"),
 *              mood (+/- n)
 *   mood     the world's mood ("mood": { "start", "min", "max", "bands": [thresholds, low to high] }): a hidden value the
 *            story moves (AddMood, the "mood" action); OnMood fires on every change, OnMoodBand when it crosses a band.
 *            Band 0 is the middle band; below it -1, -2..., above it 1, 2... The game decides what the mood looks like.
 *   text     {quest:id} -> "progress/count", plus any placeholder the game adds ({credits}, {rank}, ...)
 *
 * Checks are hidden rolls (formula: FLMCheckRules). The roll is seeded by (seed, speaker, choice), so reloading
 * can't change it, and a failed check stays failed. A check with "level" scales with the hero's level (HeroLevel):
 * below "min" it can't succeed (it goes to "tooLow" if given, else "fail"), and each level above adds "per" percent.
 * Such a check is rolled again once per level gained after a failure, so the hero can come back stronger; a success is
 * remembered for good.
 *
 * Everything game-specific is registered by the game: conditions (rank, credits...), actions (buy, open a door...), text
 * placeholders, how stats are read, what verbs cost. Loom never touches actors, UI or pausing; it raises events.
 */
UCLASS()
class LOOM_API ULMStory : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static ULMStory* Get(const UObject* WorldContext);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** The JSON object holding the story sections (usually the game's whole data file). */
	void SetData(const LMJson::FObj& InRoot);
	LMJson::FObj Entry(const FString& Section, const FString& Id) const { return LMJson::Obj(LMJson::Obj(Root, Section), Id); }

	// ---- extension points (set up by the game) ----
	using FCondition = TFunction<bool(const LMJson::FObj& Cond)>;
	using FAction = TFunction<void(const LMJson::FObj& Action)>;
	/** A condition key, e.g. "class": the handler gets the whole condition object. */
	void AddCondition(const FString& Key, FCondition Fn) { Conditions.Add(Key, MoveTemp(Fn)); }
	/** An action key, e.g. "giveItem": the handler gets the whole action object. */
	void AddAction(const FString& Key, FAction Fn) { Actions.Add(Key, MoveTemp(Fn)); }
	/** A text placeholder: {Key} is replaced by what Fn returns. */
	void AddPlaceholder(const FString& Key, TFunction<FString()> Fn) { Placeholders.Add(Key, MoveTemp(Fn)); }
	/** The hero's value for a stat named by a verb ("presence", "might"...). */
	TFunction<float(FName Stat)> StatValue;
	/** The hero's level, for level-scaled checks ("check": { "level": ... }). Unset = 1. */
	TFunction<int32()> HeroLevel;
	/** How many of an item the hero carries ("collect" objectives). */
	TFunction<int32(const FString& Item)> ItemCount;
	/** What a base price costs right now ("{price:15}" in text; the game decides: mood, haggling...). */
	TFunction<int32(int32 Base)> Price;
	/** Can the hero use this verb at all (class-only verbs)? Default: yes. */
	TFunction<bool(const LMJson::FObj& Verb)> VerbAvailable;
	/** Adjust a choice's view for its verb (cost in the label, disabled when unaffordable...). */
	TFunction<void(const LMJson::FObj& Verb, FLMChoiceView& View)> DecorateVerb;
	/** The hero picked a choice with this verb: pay for it. */
	TFunction<void(const LMJson::FObj& Verb)> PayVerb;
	/** Verb used by a check with no "verb". */
	FString DefaultVerb;
	FLMCheckRules CheckRules;
	/** Shown before an objective's count in ProgressText, per objective type (e.g. "kill" -> "Slain"). */
	TMap<FString, FString> ObjectiveLabels;

	// ---- events ----
	FLMQuestEvent OnQuest;            // started / complete / turnedIn (on turnedIn the game hands out the "reward")
	FLMOutcome OnResolved;            // an encounter was resolved
	FLMEvent OnDialogueOpened, OnDialogueClosed;
	FLMEvent OnDialogueChanged;       // new line or choices (also fires on close)
	FLMMoodEvent OnMood;              // the world's mood moved
	FLMMoodEvent OnMoodBand;          // ...into another band

	// ---- state ----
	int32 Seed = 0;
	TMap<FString, FString> Flags;          // "met_captain" -> "true", "checkpoint_outcome" -> "bribed"
	TMap<FString, float> Disposition;      // speaker key -> -100..100
	TMap<FString, bool> Checks;            // seeded check key -> passed
	TMap<FString, FString> Factions;       // "smugglers" -> "neutral" | "hostile"
	TMap<FString, FLMQuest> Quests;
	float Mood = 0.f;                      // the world's mood (hidden)

	/** Move the world's mood (clamped); Why goes to the log. */
	void AddMood(float Delta, const FString& Why = FString());
	/** The band the mood is in: 0 in the middle, negative below, positive above. */
	int32 MoodBand() const;

	bool HasFlag(const FString& Key) const { return Flags.Contains(Key); }
	void SetFlag(const FString& Key, const FString& Value = TEXT("true")) { Flags.Add(Key, Value); }
	FString Faction(const FString& Id) const { const FString* F = Factions.Find(Id); return F ? *F : TEXT("neutral"); }

	// ---- quests ----
	FString QuestStatus(const FString& Id) const { const FLMQuest* Q = Quests.Find(Id); return Q ? Q->Status : TEXT("none"); }
	void StartQuest(const FString& Id);
	void RefreshQuest(const FString& Id);
	void RefreshQuests();
	/** Something was defeated: advances "kill" objectives targeting it. */
	void OnKill(const FString& Target);
	void TurnIn(const FString& Id);
	/** A flag objective's "text", else "<label> progress/count". */
	FString ProgressText(const FString& Id) const;

	/** Mark an encounter resolved (data: its "flag" and "outcomeFlag"). Once only. */
	void Resolve(const FString& Encounter, const FString& Outcome);

	// ---- dialogue ----
	bool IsDialogueOpen() const { return bOpen; }
	/** Start talking to Who (any object the game likes; read back with Speaker()). Empty NodeId = the speaker's root. */
	void OpenDialogue(UObject* Who, const FLMSpeaker& Info, const FString& NodeId = FString());
	void Choose(int32 VisibleIndex);
	void CloseDialogue();
	UObject* Speaker() const { return SpeakerObj.Get(); }
	const FLMSpeaker& SpeakerInfo() const { return Info; }
	FString DialogueText;
	TArray<FLMChoiceView> ChoiceViews;
	bool bShowOdds = false;
	/** Self-test hook: force the next check's result (otherwise the hidden seeded roll decides). */
	TOptional<bool> ForcedCheck;
	/** Index of the first visible choice whose text starts with Prefix, or INDEX_NONE. */
	int32 FindChoice(const FString& Prefix) const { return ChoiceViews.IndexOfByPredicate([&](const FLMChoiceView& V) { return V.Text.StartsWith(Prefix); }); }

	bool CheckCond(const TSharedPtr<FJsonValue>& Cond) const;
	FString Template(const FString& Text) const;
	/** "?" (a quest to turn in), "!" (a quest on offer) or "" for a root dialogue node. */
	FString MarkerFor(const FString& RootNode) const;

	/** Run a list of data actions (as a dialogue choice's "do" would), outside a conversation too. */
	void RunActions(const TArray<TSharedPtr<FJsonValue>>& List);

private:
	void ShowNode(const FString& Id);
	bool CondObj(const LMJson::FObj& C) const;
	float CheckChance(const LMJson::FObj& Choice) const;
	bool ChoiceVisible(const LMJson::FObj& Choice) const;
	FString CheckKey(const LMJson::FObj& Choice) const;
	/** The key a check's roll is stored under: CheckKey, plus the hero's level for level-scaled checks (a new roll per level). */
	FString RollKey(const LMJson::FObj& Choice) const;
	int32 Level() const { return HeroLevel ? FMath::Max(1, HeroLevel()) : 1; }
	FString NodeText(const LMJson::FObj& Node) const;
	LMJson::FObj Verb(const LMJson::FObj& Choice) const;

	LMJson::FObj Root;
	TArray<float> MoodBands = { -60.f, -25.f, -8.f, 8.f, 25.f, 60.f };
	float MoodMin = -100.f, MoodMax = 100.f;
	TMap<FString, FCondition> Conditions;
	TMap<FString, FAction> Actions;
	TMap<FString, TFunction<FString()>> Placeholders;

	bool bOpen = false;
	FString NodeId;
	TWeakObjectPtr<UObject> SpeakerObj;
	FLMSpeaker Info;
	TArray<LMJson::FObj> VisibleChoices;
};
