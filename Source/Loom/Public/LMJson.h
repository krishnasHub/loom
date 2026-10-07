#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Small, forgiving accessors over FJsonObject. Story data is authored by hand, so a missing field falls back to a default instead of crashing.
 */
namespace LMJson
{
	using FObj = TSharedPtr<FJsonObject>;

	inline double Num(const FObj& O, const FString& Key, double Default = 0.0)
	{
		double V;
		return (O.IsValid() && O->TryGetNumberField(Key, V)) ? V : Default;
	}

	inline FString Str(const FObj& O, const FString& Key, const FString& Default = FString())
	{
		FString V;
		return (O.IsValid() && O->TryGetStringField(Key, V)) ? V : Default;
	}

	inline bool Bool(const FObj& O, const FString& Key, bool Default = false)
	{
		bool V;
		return (O.IsValid() && O->TryGetBoolField(Key, V)) ? V : Default;
	}

	inline FObj Obj(const FObj& O, const FString& Key)
	{
		const TSharedPtr<FJsonObject>* V = nullptr;
		return (O.IsValid() && O->TryGetObjectField(Key, V) && V) ? *V : FObj();
	}

	inline TArray<TSharedPtr<FJsonValue>> Arr(const FObj& O, const FString& Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* V = nullptr;
		return (O.IsValid() && O->TryGetArrayField(Key, V) && V) ? *V : TArray<TSharedPtr<FJsonValue>>();
	}

	inline bool Has(const FObj& O, const FString& Key) { return O.IsValid() && O->HasField(Key); }

	/** "#rrggbb" -> linear colour (the hex values are sRGB). */
	inline FLinearColor Color(const FString& Hex, const FLinearColor& Default = FLinearColor::White)
	{
		if (Hex.IsEmpty()) return Default;
		return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
	}
}
