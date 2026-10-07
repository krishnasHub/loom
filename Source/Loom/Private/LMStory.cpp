#include "LMStory.h"
#include "Loom.h"
#include "Engine/World.h"

ULMStory* ULMStory::Get(const UObject* WorldContext)
{
	const UWorld* W = WorldContext ? WorldContext->GetWorld() : nullptr;
	return W ? W->GetSubsystem<ULMStory>() : nullptr;
}

void ULMStory::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Seed = FMath::RandRange(1, 1000000000);
}

// ---------------------------------------------------------------------------------------------
// Quests
// ---------------------------------------------------------------------------------------------

void ULMStory::StartQuest(const FString& Id)
{
	Quests.Add(Id, FLMQuest{ TEXT("active"), 0 });
	OnQuest.Broadcast(Id, TEXT("started"));
	RefreshQuest(Id);
}

void ULMStory::RefreshQuest(const FString& Id)
{
	FLMQuest* Q = Quests.Find(Id);
	if (!Q || Q->Status == TEXT("turnedIn")) return;
	const LMJson::FObj Def = Entry(TEXT("quests"), Id);
	const LMJson::FObj O = LMJson::Obj(Def, TEXT("objective"));
	const FString Type = LMJson::Str(O, TEXT("type"));
	const int32 Count = int32(LMJson::Num(O, TEXT("count"), 1));
	if (Type == TEXT("collect")) Q->Progress = ItemCount ? FMath::Min(Count, ItemCount(LMJson::Str(O, TEXT("item")))) : 0;
	if (Type == TEXT("flag")) Q->Progress = HasFlag(LMJson::Str(O, TEXT("flag"))) ? 1 : 0;
	const bool bDone = Q->Progress >= Count;
	if (bDone && Q->Status == TEXT("active"))
	{
		Q->Status = TEXT("complete");
		OnQuest.Broadcast(Id, TEXT("complete"));
	}
	else if (!bDone && Q->Status == TEXT("complete")) Q->Status = TEXT("active");
}

void ULMStory::RefreshQuests()
{
	TArray<FString> Ids;
	Quests.GetKeys(Ids);
	for (const FString& Id : Ids) RefreshQuest(Id);
}

void ULMStory::OnKill(const FString& Target)
{
	for (auto& KV : Quests)
	{
		const LMJson::FObj O = LMJson::Obj(Entry(TEXT("quests"), KV.Key), TEXT("objective"));
		if (KV.Value.Status == TEXT("active") && LMJson::Str(O, TEXT("type")) == TEXT("kill") && LMJson::Str(O, TEXT("target")) == Target)
		{
			KV.Value.Progress = FMath::Min(int32(LMJson::Num(O, TEXT("count"), 1)), KV.Value.Progress + 1);
		}
	}
	RefreshQuests();
}

void ULMStory::TurnIn(const FString& Id)
{
	FLMQuest* Q = Quests.Find(Id);
	if (!Q) return;
	Q->Status = TEXT("turnedIn");
	OnQuest.Broadcast(Id, TEXT("turnedIn"));
}

FString ULMStory::ProgressText(const FString& Id) const
{
	const LMJson::FObj O = LMJson::Obj(Entry(TEXT("quests"), Id), TEXT("objective"));
	const FString Type = LMJson::Str(O, TEXT("type"));
	if (Type == TEXT("flag")) return LMJson::Str(O, TEXT("text"));
	const FLMQuest* Q = Quests.Find(Id);
	const FString Label = ObjectiveLabels.FindRef(Type);
	return FString::Printf(TEXT("%s%s%d/%d"), *Label, Label.IsEmpty() ? TEXT("") : TEXT(" "), Q ? Q->Progress : 0, int32(LMJson::Num(O, TEXT("count"), 1)));
}

void ULMStory::Resolve(const FString& Encounter, const FString& Outcome)
{
	const LMJson::FObj Enc = Entry(TEXT("encounters"), Encounter);
	const FString Flag = LMJson::Str(Enc, TEXT("flag"), Encounter + TEXT("_resolved"));
	if (HasFlag(Flag)) return;
	SetFlag(Flag);
	if (LMJson::Has(Enc, TEXT("outcomeFlag"))) SetFlag(LMJson::Str(Enc, TEXT("outcomeFlag")), Outcome);
	UE_LOG(LogLoom, Display, TEXT("Encounter %s resolved: %s"), *Encounter, *Outcome);
	OnResolved.Broadcast(Encounter, Outcome);
	RefreshQuests();
}

// ---------------------------------------------------------------------------------------------
// Conditions and text
// ---------------------------------------------------------------------------------------------

bool ULMStory::CheckCond(const TSharedPtr<FJsonValue>& Cond) const
{
	if (!Cond.IsValid() || Cond->IsNull()) return true;
	if (Cond->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& C : Cond->AsArray()) if (!CheckCond(C)) return false;
		return true;
	}
	return Cond->Type == EJson::Object ? CondObj(Cond->AsObject()) : true;
}

bool ULMStory::CondObj(const LMJson::FObj& C) const
{
	auto OneOf = [&](const FString& Value)
	{
		const TSharedPtr<FJsonValue> V = C->TryGetField(TEXT("is"));
		if (!V) return false;
		if (V->Type == EJson::Array) { for (const auto& X : V->AsArray()) if (X->AsString() == Value) return true; return false; }
		return V->AsString() == Value;
	};
	// Every key the object names must hold ("is" / "gte" modify the key they sit with).
	for (const auto& KV : C->Values)
	{
		const FString Key(*KV.Key);
		bool bOk = true;
		if (Key == TEXT("not")) bOk = !CheckCond(KV.Value);
		else if (Key == TEXT("quest")) bOk = OneOf(QuestStatus(KV.Value->AsString()));
		else if (Key == TEXT("flag"))
		{
			const FString* V = Flags.Find(KV.Value->AsString());
			bOk = C->HasField(TEXT("is")) ? (V && *V == LMJson::Str(C, TEXT("is"))) : V != nullptr;
		}
		else if (Key == TEXT("disposition")) bOk = Disposition.FindRef(Info.Key) >= LMJson::Num(LMJson::Obj(C, TEXT("disposition")), TEXT("gte"));
		else if (const FCondition* Fn = Conditions.Find(Key)) bOk = (*Fn)(C);
		if (!bOk) return false;
	}
	return true;
}

FString ULMStory::Template(const FString& Text) const
{
	FString Out = Text;
	for (const auto& KV : Placeholders)
	{
		const FString Tag = TEXT("{") + KV.Key + TEXT("}");
		if (Out.Contains(Tag)) Out = Out.Replace(*Tag, *KV.Value());
	}
	int32 Start;
	while ((Start = Out.Find(TEXT("{quest:"))) != INDEX_NONE)
	{
		const int32 End = Out.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE) break;
		const FString Id = Out.Mid(Start + 7, End - Start - 7);
		const FLMQuest* Q = Quests.Find(Id);
		const int32 Count = int32(LMJson::Num(LMJson::Obj(Entry(TEXT("quests"), Id), TEXT("objective")), TEXT("count"), 1));
		Out = Out.Left(Start) + FString::Printf(TEXT("%d/%d"), Q ? Q->Progress : 0, Count) + Out.Mid(End + 1);
	}
	return Out;
}

FString ULMStory::MarkerFor(const FString& RootNode) const
{
	if (RootNode.IsEmpty()) return FString();
	FString Marker;
	for (const TSharedPtr<FJsonValue>& V : LMJson::Arr(Entry(TEXT("dialogue"), RootNode), TEXT("choices")))
	{
		const LMJson::FObj C = V->AsObject();
		const FString M = LMJson::Str(C, TEXT("marker"));
		if (M.IsEmpty() || !CheckCond(C->TryGetField(TEXT("if")))) continue;
		if (M == TEXT("?")) return M;
		Marker = M;
	}
	return Marker;
}

FString ULMStory::NodeText(const LMJson::FObj& Node) const
{
	const TSharedPtr<FJsonValue> T = Node->TryGetField(TEXT("text"));
	if (!T) return FString();
	if (T->Type != EJson::Array) return T->AsString();
	const TArray<TSharedPtr<FJsonValue>>& Variants = T->AsArray();
	for (const TSharedPtr<FJsonValue>& V : Variants)
	{
		const LMJson::FObj O = V->AsObject();
		if (CheckCond(O->TryGetField(TEXT("if")))) return LMJson::Str(O, TEXT("text"));
	}
	return Variants.Num() ? LMJson::Str(Variants.Last()->AsObject(), TEXT("text")) : FString();
}

// ---------------------------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------------------------

namespace
{
	// Seeded roll: same seed + same speaker + same option => same result (no reload-scumming).
	float SeededRandom(const FString& Key)
	{
		uint32 H = 2166136261u;
		for (TCHAR C : Key) { H ^= uint32(C); H *= 16777619u; }
		uint32 T = H + 0x6D2B79F5u;
		T = (T ^ (T >> 15)) * (T | 1u);
		T ^= T + (T ^ (T >> 7)) * (T | 61u);
		return float((T ^ (T >> 14)) & 0xFFFFFF) / float(0x1000000);
	}
}

LMJson::FObj ULMStory::Verb(const LMJson::FObj& Choice) const
{
	return Entry(TEXT("dialogueVerbs"), LMJson::Str(Choice, TEXT("verb"), DefaultVerb));
}

FString ULMStory::CheckKey(const LMJson::FObj& Choice) const
{
	return FString::Printf(TEXT("%d|%s|%s"), Seed, *Info.Key, *LMJson::Str(Choice, TEXT("_key")));
}

float ULMStory::CheckChance(const LMJson::FObj& Choice) const
{
	const LMJson::FObj Check = LMJson::Obj(Choice, TEXT("check"));
	const LMJson::FObj V = Verb(Choice);
	auto Stat = [&](const FString& Name) { return StatValue && !Name.IsEmpty() ? StatValue(FName(Name)) : 0.f; };
	const FLMCheckRules& R = CheckRules;
	const float Score = Stat(LMJson::Str(V, TEXT("stat"))) + Stat(LMJson::Str(V, TEXT("secondary"))) * R.SecondaryWeight;
	float Pct = R.Base + R.PerPoint * (Score - float(LMJson::Num(Check, TEXT("resolve"), R.DefaultResolve))) + float(LMJson::Num(V, TEXT("bonus"), 0))
		+ Disposition.FindRef(Info.Key) / FMath::Max(R.DispositionDiv, 0.001f);
	for (const TSharedPtr<FJsonValue>& B : LMJson::Arr(Check, TEXT("bonus")))
		if (CheckCond(B->AsObject()->TryGetField(TEXT("if")))) Pct += float(LMJson::Num(B->AsObject(), TEXT("add"), 0));
	return FMath::Clamp(Pct, R.Min, R.Max) / 100.f;
}

bool ULMStory::ChoiceVisible(const LMJson::FObj& Choice) const
{
	if (LMJson::Has(Choice, TEXT("verb")) && VerbAvailable && !VerbAvailable(Verb(Choice))) return false;
	if (LMJson::Has(Choice, TEXT("check")))
	{
		const bool* Done = Checks.Find(CheckKey(Choice));
		if (Done && !*Done) return false;   // a failed check stays failed
	}
	return CheckCond(Choice->TryGetField(TEXT("if")));
}

// ---------------------------------------------------------------------------------------------
// Dialogue
// ---------------------------------------------------------------------------------------------

void ULMStory::OpenDialogue(UObject* Who, const FLMSpeaker& InInfo, const FString& InNodeId)
{
	SpeakerObj = Who;
	Info = InInfo;
	bOpen = true;
	OnDialogueOpened.Broadcast();
	ShowNode(InNodeId.IsEmpty() ? Info.Root : InNodeId);
}

void ULMStory::ShowNode(const FString& Id)
{
	const LMJson::FObj Node = Entry(TEXT("dialogue"), Id);
	if (Id.IsEmpty() || !Node) { CloseDialogue(); return; }
	NodeId = Id;

	const FString Text = Template(NodeText(Node));   // pick the text before the node's actions run
	RunActions(LMJson::Arr(Node, TEXT("do")));
	if (!bOpen) return;
	DialogueText = Text;

	VisibleChoices.Reset();
	ChoiceViews.Reset();
	const TArray<TSharedPtr<FJsonValue>> Choices = LMJson::Arr(Node, TEXT("choices"));
	for (int32 I = 0; I < Choices.Num(); ++I)
	{
		const LMJson::FObj C = Choices[I]->AsObject();
		C->SetStringField(TEXT("_key"), FString::Printf(TEXT("%s#%d"), *Id, I));
		if (!ChoiceVisible(C)) continue;
		VisibleChoices.Add(C);

		FLMChoiceView View;
		View.Text = Template(LMJson::Str(C, TEXT("text")));
		if (LMJson::Has(C, TEXT("verb")))
		{
			const LMJson::FObj V = Verb(C);
			View.VerbId = LMJson::Str(C, TEXT("verb"));
			View.Verb = LMJson::Str(V, TEXT("label"), View.VerbId);
			if (DecorateVerb) DecorateVerb(V, View);
		}
		if (bShowOdds && LMJson::Has(C, TEXT("check"))) View.Odds = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(CheckChance(C) * 100.f));
		ChoiceViews.Add(View);
	}
	OnDialogueChanged.Broadcast();
}

void ULMStory::Choose(int32 Index)
{
	if (!bOpen || !VisibleChoices.IsValidIndex(Index) || !ChoiceViews[Index].bEnabled) return;
	const LMJson::FObj C = VisibleChoices[Index];
	if (LMJson::Has(C, TEXT("verb")) && PayVerb) PayVerb(Verb(C));

	FString Next = LMJson::Str(C, TEXT("next"));
	if (const LMJson::FObj Check = LMJson::Obj(C, TEXT("check")))
	{
		const FString Key = CheckKey(C);
		if (ForcedCheck.IsSet()) { Checks.Add(Key, ForcedCheck.GetValue()); ForcedCheck.Reset(); }
		if (!Checks.Contains(Key)) Checks.Add(Key, SeededRandom(Key) < CheckChance(C));
		Next = Checks[Key] ? LMJson::Str(Check, TEXT("success")) : LMJson::Str(Check, TEXT("fail"));
		UE_LOG(LogLoom, Display, TEXT("Dialogue check %s (%s): %s"), *Key, *LMJson::Str(C, TEXT("verb"), DefaultVerb), Checks[Key] ? TEXT("success") : TEXT("fail"));
	}
	RunActions(LMJson::Arr(C, TEXT("do")));
	if (!bOpen) return;
	if (Next.IsEmpty()) CloseDialogue(); else ShowNode(Next);
}

void ULMStory::CloseDialogue()
{
	if (!bOpen) return;
	bOpen = false;
	OnDialogueClosed.Broadcast();
	OnDialogueChanged.Broadcast();
}

void ULMStory::RunActions(const TArray<TSharedPtr<FJsonValue>>& List)
{
	for (const TSharedPtr<FJsonValue>& V : List)
	{
		const LMJson::FObj A = V->AsObject();
		if (!A || !CheckCond(A->TryGetField(TEXT("if")))) continue;
		for (const auto& KV : A->Values)
		{
			const FString Key(*KV.Key);
			if (Key == TEXT("if")) continue;
			if (Key == TEXT("startQuest")) StartQuest(KV.Value->AsString());
			else if (Key == TEXT("turnIn")) TurnIn(KV.Value->AsString());
			else if (Key == TEXT("setFlag")) SetFlag(KV.Value->AsString());
			else if (Key == TEXT("disposition")) { float& D = Disposition.FindOrAdd(Info.Key); D = FMath::Clamp(D + float(KV.Value->AsNumber()), CheckRules.DispositionMin, CheckRules.DispositionMax); }
			else if (Key == TEXT("resolve")) { FString Enc, Outcome; if (KV.Value->AsString().Split(TEXT(":"), &Enc, &Outcome)) Resolve(Enc, Outcome); }
			else if (const FAction* Fn = Actions.Find(Key)) (*Fn)(A);
			else UE_LOG(LogLoom, Warning, TEXT("Unknown dialogue action \"%s\""), *Key);
		}
	}
	RefreshQuests();
}
