# Loom

A small story engine for Unreal Engine 5 (C++ only, no assets): branching dialogue with hidden seeded checks,
flags, disposition, factions, quests and encounter outcomes. Story content is JSON; anything specific to a game
(what a hero is, what an item is, what "start a duel" does) is registered by that game.

Loom depends only on the engine. It knows nothing about actors, UI, input or pausing: it keeps the story state,
runs the dialogue, and raises events.

## Add it to a game

```
git submodule add https://github.com/krishnasHub/loom.git <Project>/Plugins/Loom
```

Enable it in the `.uproject` (`{ "Name": "Loom", "Enabled": true }`) and add `"Loom"` to the game module's
dependencies. `ULMStory` is a world subsystem: `ULMStory::Get(WorldContext)`.

## Wire it up (once per world)

```cpp
ULMStory* L = Collection.InitializeDependency<ULMStory>();   // from your own world subsystem's Initialize
L->SetData(MyJsonRoot);                                      // holds "dialogue", "dialogueVerbs", "quests", "encounters"
L->DefaultVerb = TEXT("persuade");

L->StatValue = [this](FName Stat) { return Hero()->GetStat(Stat); };            // for checks
L->ItemCount = [this](const FString& Item) { return Hero()->CountItem(Item); };  // "collect" objectives
L->AddCondition(TEXT("credits"), [this](const LMJson::FObj& C) { return Hero()->Credits >= LMJson::Num(C, TEXT("credits")); });
L->AddAction(TEXT("hack"), [this](const LMJson::FObj& A) { OpenDoor(LMJson::Str(A, TEXT("hack"))); });
L->AddPlaceholder(TEXT("callsign"), [this]() { return Hero()->Callsign; });

L->OnDialogueOpened.AddLambda([this]() { /* pause, show the dialogue UI */ });
L->OnQuest.AddLambda([this](const FString& Id, FName What) { /* toast; on "turnedIn" hand out the reward */ });

L->OpenDialogue(Npc, FLMSpeaker{ Name, Color, TEXT("guard_captain"), TEXT("captain_root") });
L->Choose(0);   // the UI shows L->DialogueText and L->ChoiceViews
```

## Data

```jsonc
"dialogue": {
  "captain_root": {
    "text": [ { "if": { "flag": "met" }, "text": "You again." }, { "text": "Halt. {callsign}, is it?" } ],
    "do": [ { "setFlag": "met" } ],
    "choices": [
      { "text": "Let me through.", "verb": "persuade",
        "check": { "resolve": 6, "bonus": [ { "if": { "flag": "has_pass" }, "add": 20 } ],
                   "success": "captain_ok", "fail": "captain_no" } },
      { "text": "[Pay 50]", "if": { "credits": 50 }, "do": [ { "resolve": "checkpoint:paid" } ] }
    ]
  }
},
"dialogueVerbs": { "persuade": { "label": "Persuade", "stat": "presence", "bonus": 0 } },
"quests": { "find_pass": { "name": "...", "objective": { "type": "kill" | "collect" | "flag", ... }, "reward": { ... } } },
"encounters": { "checkpoint": { "flag": "checkpoint_done", "outcomeFlag": "checkpoint_outcome" } }
```

Built in: conditions `not`, `quest` + `is`, `flag` (+ `is`), `disposition` + `gte`; actions `startQuest`,
`turnIn`, `setFlag`, `disposition`, `resolve`; text `{quest:id}`. A condition object needs all its keys to hold;
an array of them needs all of them.

Checks are hidden rolls seeded by (seed, speaker, choice): reloading gives the same result, and a failed check
stays failed. The formula's numbers are in `ULMStory::CheckRules`.
