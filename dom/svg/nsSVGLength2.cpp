/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ArrayUtils.h"

#include "nsSVGLength2.h"
#include "mozilla/dom/SVGAnimatedLength.h"
#include "mozilla/dom/SVGSVGElement.h"
#include "nsContentUtils.h" // NS_ENSURE_FINITE
#include "nsIFrame.h"
#include "nsSMILFloatType.h"
#include "nsSMILValue.h"
#include "nsSVGAttrTearoffTable.h"
#include "nsSVGIntegrationUtils.h"
#include "nsTextFormatter.h"
#include "DOMSVGLength.h"
#include "LayoutLogging.h"

using namespace mozilla;
using namespace mozilla::dom;

static nsIAtom** const unitMap[] =
{
  nullptr, /* SVG_LENGTHTYPE_UNKNOWN */
  nullptr, /* SVG_LENGTHTYPE_NUMBER */
  &nsGkAtoms::percentage,
  &nsGkAtoms::em,
  &nsGkAtoms::ex,
  &nsGkAtoms::px,
  &nsGkAtoms::cm,
  &nsGkAtoms::mm,
  &nsGkAtoms::in,
  &nsGkAtoms::pt,
  &nsGkAtoms::pc
};

static nsSVGAttrTearoffTable<nsSVGLength2, SVGAnimatedLength>
  sSVGAnimatedLengthTearoffTable;

/* Helper functions */

static bool
IsValidUnitType(uint16_t unit)
{
  if (unit > nsIDOMSVGLength::SVG_LENGTHTYPE_UNKNOWN &&
      unit <= nsIDOMSVGLength::SVG_LENGTHTYPE_PC)
    return true;

  return false;
}

static void
GetUnitString(nsAString& unit, uint16_t unitType)
{
  if (IsValidUnitType(unitType)) {
    if (unitMap[unitType]) {
      (*unitMap[unitType])->ToString(unit);
    }
    return;
  }

  NS_NOTREACHED("Unknown unit type");
  return;
}

static uint16_t
GetUnitTypeForString(const nsAString& unitStr)
{
  if (unitStr.IsEmpty()) 
    return nsIDOMSVGLength::SVG_LENGTHTYPE_NUMBER;
                   
  nsIAtom *unitAtom = NS_GetStaticAtom(unitStr);
  if (unitAtom) {
    for (uint32_t i = 0 ; i < ArrayLength(unitMap) ; i++) {
      if (unitMap[i] && *unitMap[i] == unitAtom) {
        return i;
      }
    }
  }

  return nsIDOMSVGLength::SVG_LENGTHTYPE_UNKNOWN;
}

static void
GetValueString(nsAString &aValueAsString, float aValue, uint16_t aUnitType)
{
  char16_t buf[24];
  nsTextFormatter::snprintf(buf, sizeof(buf)/sizeof(char16_t),
                            u"%g",
                            (double)aValue);
  aValueAsString.Assign(buf);

  nsAutoString unitString;
  GetUnitString(unitString, aUnitType);
  aValueAsString.Append(unitString);
}

static bool
GetValueFromString(const nsAString& aString,
                   float& aValue,
                   uint16_t* aUnitType)
{
  RangedPtr<const char16_t> iter =
    SVGContentUtils::GetStartRangedPtr(aString);
  const RangedPtr<const char16_t> end =
    SVGContentUtils::GetEndRangedPtr(aString);

  if (!SVGContentUtils::ParseNumber(iter, end, aValue)) {
    return false;
  }
  const nsAString& units = Substring(iter.get(), end.get());
  *aUnitType = GetUnitTypeForString(units);
  return IsValidUnitType(*aUnitType);
}

// Parser for calc() expressions in SVG length attributes, e.g.
// "calc(100% - 2px)". Supports sums and products of <number> and
// <percentage> terms (a unitless number counts as user units, matching SVG
// attribute syntax). The expression is reduced to a percentage component and
// an absolute (user unit) component that are resolved against the element's
// metrics when the length is used. Returns false for anything else so that
// the existing syntax-error path is preserved.
namespace {

class CalcExprParser
{
public:
  CalcExprParser(const char16_t* aPos, const char16_t* aEnd)
    : mPos(aPos), mEnd(aEnd)
  {
  }

  bool Parse(double& aPercent, double& aAbsPx)
  {
    if (!ParseSum(aPercent, aAbsPx) || !AtEnd()) {
      return false;
    }
    return mozilla::IsFinite(aPercent) && mozilla::IsFinite(aAbsPx);
  }

private:
  bool AtEnd() const { return mPos == mEnd; }

  void SkipWs()
  {
    while (mPos < mEnd && SVGContentUtils::IsSVGWhitespace(*mPos)) {
      ++mPos;
    }
  }

  bool ParseSum(double& aPercent, double& aAbsPx)
  {
    if (!ParseProduct(aPercent, aAbsPx)) {
      return false;
    }
    while (true) {
      SkipWs();
      if (AtEnd()) {
        return true;
      }
      char16_t op = *mPos;
      if (op != '+' && op != '-') {
        return true;
      }
      ++mPos;
      double p2, x2;
      if (!ParseProduct(p2, x2)) {
        return false;
      }
      if (op == '+') {
        aPercent += p2;
        aAbsPx += x2;
      } else {
        aPercent -= p2;
        aAbsPx -= x2;
      }
    }
  }

  bool ParseProduct(double& aPercent, double& aAbsPx)
  {
    if (!ParseUnary(aPercent, aAbsPx)) {
      return false;
    }
    while (true) {
      SkipWs();
      if (AtEnd()) {
        return true;
      }
      char16_t op = *mPos;
      if (op != '*' && op != '/') {
        return true;
      }
      ++mPos;
      double p2, x2;
      if (!ParseUnary(p2, x2)) {
        return false;
      }
      if (op == '*') {
        // At most one side may carry a percentage; the percentage-free side
        // acts as the scalar multiplier.
        if (aPercent != 0 && p2 != 0) {
          return false;
        }
        if (aPercent == 0) {
          aPercent = aAbsPx * p2;
          aAbsPx *= x2;
        } else {
          // The right side is percentage-free, so it scales both components.
          aPercent *= x2;
          aAbsPx *= x2;
        }
      } else {
        // Division is only valid by a percentage-free term.
        if (p2 != 0 || x2 == 0) {
          return false;
        }
        aPercent /= x2;
        aAbsPx /= x2;
      }
    }
  }

  bool ParseUnary(double& aPercent, double& aAbsPx)
  {
    SkipWs();
    if (AtEnd()) {
      return false;
    }
    char16_t op = *mPos;
    if (op == '+' || op == '-') {
      ++mPos;
      if (!ParseUnary(aPercent, aAbsPx)) {
        return false;
      }
      if (op == '-') {
        aPercent = -aPercent;
        aAbsPx = -aAbsPx;
      }
      return true;
    }
    return ParseAtom(aPercent, aAbsPx);
  }

  bool ParseAtom(double& aPercent, double& aAbsPx)
  {
    if (!AtEnd() && *mPos == '(') {
      ++mPos;
      if (!ParseSum(aPercent, aAbsPx)) {
        return false;
      }
      SkipWs();
      if (AtEnd() || *mPos != ')') {
        return false;
      }
      ++mPos;
      return true;
    }

    RangedPtr<const char16_t> iter(mPos, mEnd);
    RangedPtr<const char16_t> iterEnd(mEnd, mEnd);
    double value;
    if (!SVGContentUtils::ParseNumber(iter, iterEnd, value)) {
      return false;
    }
    mPos = iter.get();

    if (mPos < mEnd && *mPos == '%') {
      ++mPos;
      aPercent = value;
      aAbsPx = 0;
      return true;
    }

    // Consume an optional "px" (user unit) suffix; any other unit inside a
    // calc() expression is left in place and rejected by the caller.
    if (mEnd - mPos >= 2 &&
        ((*mPos == 'p' || *mPos == 'P') && (mPos[1] == 'x' || mPos[1] == 'X'))) {
      mPos += 2;
    }
    aPercent = 0;
    aAbsPx = value;
    return true;
  }

  const char16_t* mPos;
  const char16_t* mEnd;
};

} // namespace

static bool
ParseCalcLengthString(const nsAString& aString, float& aPercent, float& aAbsPx)
{
  const char16_t* p = aString.BeginReading();
  const char16_t* end = aString.EndReading();
  while (p < end && SVGContentUtils::IsSVGWhitespace(*p)) {
    ++p;
  }
  while (end > p && SVGContentUtils::IsSVGWhitespace(end[-1])) {
    --end;
  }

  static const char16_t kCalcPrefix[] = { 'c', 'a', 'l', 'c', '(' };
  if (end - p < 7) { // "calc()" plus at least one term
    return false;
  }
  for (uint32_t i = 0; i < ArrayLength(kCalcPrefix); ++i) {
    char16_t c = p[i];
    if (c != kCalcPrefix[i] &&
        !(c >= 'A' && c <= 'Z' && c + ('a' - 'A') == kCalcPrefix[i])) {
      return false;
    }
  }
  if (end[-1] != ')') {
    return false;
  }

  CalcExprParser parser(p + ArrayLength(kCalcPrefix), end - 1);
  double percent, absPx;
  if (!parser.Parse(percent, absPx)) {
    return false;
  }
  aPercent = float(percent);
  aAbsPx = float(absPx);
  return true;
}

static float GetMMPerPixel() { return MM_PER_INCH_FLOAT / 96; }

static float
FixAxisLength(float aLength)
{
  if (aLength == 0.0f) {
    LAYOUT_WARNING("zero axis length");
    return 1e-20f;
  }
  return aLength;
}

SVGElementMetrics::SVGElementMetrics(nsSVGElement* aSVGElement,
                                     SVGSVGElement* aCtx)
  : mSVGElement(aSVGElement)
  , mCtx(aCtx)
{
}

float
SVGElementMetrics::GetEmLength() const
{
  return SVGContentUtils::GetFontSize(mSVGElement);
}

float
SVGElementMetrics::GetExLength() const
{
  return SVGContentUtils::GetFontXHeight(mSVGElement);
}

float
SVGElementMetrics::GetAxisLength(uint8_t aCtxType) const
{
  if (!EnsureCtx()) {
    return 1;
  }

  return FixAxisLength(mCtx->GetLength(aCtxType));
}

bool
SVGElementMetrics::EnsureCtx() const
{
  if (!mCtx && mSVGElement) {
    mCtx = mSVGElement->GetCtx();
    if (!mCtx && mSVGElement->IsSVGElement(nsGkAtoms::svg)) {
      // mSVGElement must be the outer svg element
      mCtx = static_cast<SVGSVGElement*>(mSVGElement);
    }
  }
  return mCtx != nullptr;
}

NonSVGFrameUserSpaceMetrics::NonSVGFrameUserSpaceMetrics(nsIFrame* aFrame)
  : mFrame(aFrame)
{
}

float
NonSVGFrameUserSpaceMetrics::GetEmLength() const
{
  return SVGContentUtils::GetFontSize(mFrame);
}

float
NonSVGFrameUserSpaceMetrics::GetExLength() const
{
  return SVGContentUtils::GetFontXHeight(mFrame);
}

gfx::Size
NonSVGFrameUserSpaceMetrics::GetSize() const
{
  return nsSVGIntegrationUtils::GetSVGCoordContextForNonSVGFrame(mFrame);
}

float
UserSpaceMetricsWithSize::GetAxisLength(uint8_t aCtxType) const
{
  gfx::Size size = GetSize();
  float length;
  switch (aCtxType) {
  case SVGContentUtils::X:
    length = size.width;
    break;
  case SVGContentUtils::Y:
    length = size.height;
    break;
  case SVGContentUtils::XY:
    length = SVGContentUtils::ComputeNormalizedHypotenuse(size.width, size.height);
    break;
  default:
    NS_NOTREACHED("Unknown axis type");
    length = 1;
    break;
  }
  return FixAxisLength(length);
}

float
nsSVGLength2::GetUnitScaleFactor(nsSVGElement *aSVGElement,
                                 uint8_t aUnitType) const
{
  return GetUnitScaleFactor(SVGElementMetrics(aSVGElement), aUnitType);
}

float
nsSVGLength2::GetUnitScaleFactor(SVGSVGElement *aCtx, uint8_t aUnitType) const
{
  return GetUnitScaleFactor(SVGElementMetrics(aCtx, aCtx), aUnitType);
}

float
nsSVGLength2::GetUnitScaleFactor(nsIFrame *aFrame, uint8_t aUnitType) const
{
  nsIContent* content = aFrame->GetContent();
  if (content->IsSVGElement()) {
    return GetUnitScaleFactor(SVGElementMetrics(static_cast<nsSVGElement*>(content)), aUnitType);
  }
  return GetUnitScaleFactor(NonSVGFrameUserSpaceMetrics(aFrame), aUnitType);
}

float
nsSVGLength2::GetAnimValue(nsIFrame *aFrame) const
{
  nsIContent* content = aFrame->GetContent();
  if (content->IsSVGElement()) {
    return GetAnimValue(SVGElementMetrics(static_cast<nsSVGElement*>(content)));
  }
  return GetAnimValue(NonSVGFrameUserSpaceMetrics(aFrame));
}

float
nsSVGLength2::GetAnimValue(SVGSVGElement *aCtx) const
{
  return GetAnimValue(SVGElementMetrics(aCtx, aCtx));
}

float
nsSVGLength2::GetBaseValue(SVGSVGElement *aCtx) const
{
  return GetBaseValue(SVGElementMetrics(aCtx, aCtx));
}

float
nsSVGLength2::GetUnitScaleFactor(const UserSpaceMetrics& aMetrics, uint8_t aUnitType) const
{
  switch (aUnitType) {
  case nsIDOMSVGLength::SVG_LENGTHTYPE_NUMBER:
  case nsIDOMSVGLength::SVG_LENGTHTYPE_PX:
    return 1;
  case nsIDOMSVGLength::SVG_LENGTHTYPE_MM:
    return GetMMPerPixel();
  case nsIDOMSVGLength::SVG_LENGTHTYPE_CM:
    return GetMMPerPixel() / 10.0f;
  case nsIDOMSVGLength::SVG_LENGTHTYPE_IN:
    return GetMMPerPixel() / MM_PER_INCH_FLOAT;
  case nsIDOMSVGLength::SVG_LENGTHTYPE_PT:
    return GetMMPerPixel() * POINTS_PER_INCH_FLOAT / MM_PER_INCH_FLOAT;
  case nsIDOMSVGLength::SVG_LENGTHTYPE_PC:
    return GetMMPerPixel() * POINTS_PER_INCH_FLOAT / MM_PER_INCH_FLOAT / 12.0f;
  case nsIDOMSVGLength::SVG_LENGTHTYPE_PERCENTAGE:
    return 100.0f / aMetrics.GetAxisLength(mCtxType);
  case nsIDOMSVGLength::SVG_LENGTHTYPE_EMS:
    return 1 / aMetrics.GetEmLength();
  case nsIDOMSVGLength::SVG_LENGTHTYPE_EXS:
    return 1 / aMetrics.GetExLength();
  default:
    NS_NOTREACHED("Unknown unit type");
    return 0;
  }
}

void
nsSVGLength2::SetBaseValueInSpecifiedUnits(float aValue,
                                           nsSVGElement *aSVGElement,
                                           bool aDoSetAttr)
{
  if (mIsBaseSet && mBaseVal == aValue) {
    return;
  }

  nsAttrValue emptyOrOldValue;
  if (aDoSetAttr) {
    emptyOrOldValue = aSVGElement->WillChangeLength(mAttrEnum);
  }
  mBaseVal = aValue;
  mIsBaseSet = true;
  mIsCalc = false;
  if (!mIsAnimated) {
    mAnimVal = mBaseVal;
  }
  else {
    aSVGElement->AnimationNeedsResample();
  }
  if (aDoSetAttr) {
    aSVGElement->DidChangeLength(mAttrEnum, emptyOrOldValue);
  }
}

nsresult
nsSVGLength2::ConvertToSpecifiedUnits(uint16_t unitType,
                                      nsSVGElement *aSVGElement)
{
  if (!IsValidUnitType(unitType))
    return NS_ERROR_DOM_NOT_SUPPORTED_ERR;

  if (mIsBaseSet && mSpecifiedUnitType == uint8_t(unitType))
    return NS_OK;

  // Even though we're not changing the visual effect this length will have
  // on the document, we still need to send out notifications in case we have
  // mutation listeners, since the actual string value of the attribute will
  // change.
  nsAttrValue emptyOrOldValue = aSVGElement->WillChangeLength(mAttrEnum);

  float valueInUserUnits =
    mBaseVal / GetUnitScaleFactor(aSVGElement, mSpecifiedUnitType);
  mSpecifiedUnitType = uint8_t(unitType);
  // Setting aDoSetAttr to false here will ensure we don't call
  // Will/DidChangeAngle a second time (and dispatch duplicate notifications).
  SetBaseValue(valueInUserUnits, aSVGElement, false);

  aSVGElement->DidChangeLength(mAttrEnum, emptyOrOldValue);

  return NS_OK;
}

nsresult
nsSVGLength2::NewValueSpecifiedUnits(uint16_t unitType,
                                     float valueInSpecifiedUnits,
                                     nsSVGElement *aSVGElement)
{
  NS_ENSURE_FINITE(valueInSpecifiedUnits, NS_ERROR_ILLEGAL_VALUE);

  if (!IsValidUnitType(unitType))
    return NS_ERROR_DOM_NOT_SUPPORTED_ERR;

  if (mIsBaseSet && mBaseVal == valueInSpecifiedUnits &&
      mSpecifiedUnitType == uint8_t(unitType)) {
    return NS_OK;
  }

  nsAttrValue emptyOrOldValue = aSVGElement->WillChangeLength(mAttrEnum);
  mBaseVal = valueInSpecifiedUnits;
  mIsBaseSet = true;
  mIsCalc = false;
  mSpecifiedUnitType = uint8_t(unitType);
  if (!mIsAnimated) {
    mAnimVal = mBaseVal;
  }
  else {
    aSVGElement->AnimationNeedsResample();
  }
  aSVGElement->DidChangeLength(mAttrEnum, emptyOrOldValue);
  return NS_OK;
}

nsresult
nsSVGLength2::ToDOMBaseVal(DOMSVGLength **aResult, nsSVGElement *aSVGElement)
{
  RefPtr<DOMSVGLength> domBaseVal =
    DOMSVGLength::GetTearOff(this, aSVGElement, false);

  domBaseVal.forget(aResult);
  return NS_OK;
}

nsresult
nsSVGLength2::ToDOMAnimVal(DOMSVGLength **aResult, nsSVGElement *aSVGElement)
{
  RefPtr<DOMSVGLength> domAnimVal =
    DOMSVGLength::GetTearOff(this, aSVGElement, true);

  domAnimVal.forget(aResult);
  return NS_OK;
}

/* Implementation */

nsresult
nsSVGLength2::SetBaseValueString(const nsAString &aValueAsString,
                                 nsSVGElement *aSVGElement,
                                 bool aDoSetAttr)
{
  float value;
  uint16_t unitType;

  if (!GetValueFromString(aValueAsString, value, &unitType)) {
    // Not a plain SVG length; try a calc() expression before failing, so
    // that markup like width="calc(100% - 2px)" is honored instead of
    // being dropped with a parse failure.
    float calcPercent, calcAbsPx;
    if (!ParseCalcLengthString(aValueAsString, calcPercent, calcAbsPx)) {
      return NS_ERROR_DOM_SYNTAX_ERR;
    }
    if (mIsBaseSet && mIsCalc && mCalcPercent == calcPercent &&
        mCalcAbsPx == calcAbsPx) {
      return NS_OK;
    }

    nsAttrValue emptyOrOldValue;
    if (aDoSetAttr) {
      emptyOrOldValue = aSVGElement->WillChangeLength(mAttrEnum);
    }
    mIsCalc = true;
    mCalcPercent = calcPercent;
    mCalcAbsPx = calcAbsPx;
    // Calc values resolve against element metrics when used. Keep the unit
    // based fields consistent for the code paths that don't know about calc.
    mBaseVal = 0;
    mSpecifiedUnitType = nsIDOMSVGLength::SVG_LENGTHTYPE_NUMBER;
    mIsBaseSet = true;
    if (!mIsAnimated) {
      mAnimVal = mBaseVal;
    }
    else {
      aSVGElement->AnimationNeedsResample();
    }
    if (aDoSetAttr) {
      aSVGElement->DidChangeLength(mAttrEnum, emptyOrOldValue);
    }
    return NS_OK;
  }

  if (mIsBaseSet && mBaseVal == float(value) &&
      mSpecifiedUnitType == uint8_t(unitType) && !mIsCalc) {
    return NS_OK;
  }

  nsAttrValue emptyOrOldValue;
  if (aDoSetAttr) {
    emptyOrOldValue = aSVGElement->WillChangeLength(mAttrEnum);
  }
  mBaseVal = value;
  mIsBaseSet = true;
  mIsCalc = false;
  mSpecifiedUnitType = uint8_t(unitType);
  if (!mIsAnimated) {
    mAnimVal = mBaseVal;
  }
  else {
    aSVGElement->AnimationNeedsResample();
  }

  if (aDoSetAttr) {
    aSVGElement->DidChangeLength(mAttrEnum, emptyOrOldValue);
  }
  return NS_OK;
}

void
nsSVGLength2::GetBaseValueString(nsAString & aValueAsString) const
{
  if (mIsCalc) {
    char16_t buf[64];
    nsTextFormatter::snprintf(buf, ArrayLength(buf),
                              u"calc(%g%% %s %gpx)",
                              (double)mCalcPercent,
                              mCalcAbsPx < 0 ? u"-" : u"+",
                              (double)(mCalcAbsPx < 0 ? -mCalcAbsPx : mCalcAbsPx));
    aValueAsString.Assign(buf);
    return;
  }
  GetValueString(aValueAsString, mBaseVal, mSpecifiedUnitType);
}

void
nsSVGLength2::GetAnimValueString(nsAString & aValueAsString) const
{
  if (mIsCalc && !mIsAnimated) {
    GetBaseValueString(aValueAsString);
    return;
  }
  if (mIsCalc) {
    // Animated calc lengths are stored as user units.
    GetValueString(aValueAsString, mAnimVal,
                   nsIDOMSVGLength::SVG_LENGTHTYPE_NUMBER);
    return;
  }
  GetValueString(aValueAsString, mAnimVal, mSpecifiedUnitType);
}

void
nsSVGLength2::SetBaseValue(float aValue, nsSVGElement *aSVGElement,
                           bool aDoSetAttr)
{
  SetBaseValueInSpecifiedUnits(aValue * GetUnitScaleFactor(aSVGElement,
                                                           mSpecifiedUnitType),
                               aSVGElement, aDoSetAttr);
}

void
nsSVGLength2::SetAnimValueInSpecifiedUnits(float aValue,
                                           nsSVGElement* aSVGElement)
{
  if (mAnimVal == aValue && mIsAnimated) {
    return;
  }
  mAnimVal = aValue;
  mIsAnimated = true;
  aSVGElement->DidAnimateLength(mAttrEnum);
}

void
nsSVGLength2::SetAnimValue(float aValue, nsSVGElement *aSVGElement)
{
  SetAnimValueInSpecifiedUnits(aValue * GetUnitScaleFactor(aSVGElement,
                                                           mSpecifiedUnitType),
                               aSVGElement);
}

already_AddRefed<SVGAnimatedLength>
nsSVGLength2::ToDOMAnimatedLength(nsSVGElement* aSVGElement)
{
  RefPtr<SVGAnimatedLength> svgAnimatedLength =
    sSVGAnimatedLengthTearoffTable.GetTearoff(this);
  if (!svgAnimatedLength) {
    svgAnimatedLength = new SVGAnimatedLength(this, aSVGElement);
    sSVGAnimatedLengthTearoffTable.AddTearoff(this, svgAnimatedLength);
  }

  return svgAnimatedLength.forget();
}

SVGAnimatedLength::~SVGAnimatedLength()
{
  sSVGAnimatedLengthTearoffTable.RemoveTearoff(mVal);
}

nsISMILAttr*
nsSVGLength2::ToSMILAttr(nsSVGElement *aSVGElement)
{
  return new SMILLength(this, aSVGElement);
}

nsresult
nsSVGLength2::SMILLength::ValueFromString(const nsAString& aStr,
                                 const SVGAnimationElement* /*aSrcElement*/,
                                 nsSMILValue& aValue,
                                 bool& aPreventCachingOfSandwich) const
{
  float value;
  uint16_t unitType;
  
  if (!GetValueFromString(aStr, value, &unitType)) {
    return NS_ERROR_DOM_SYNTAX_ERR;
  }

  nsSMILValue val(nsSMILFloatType::Singleton());
  val.mU.mDouble = value / mVal->GetUnitScaleFactor(mSVGElement, unitType);
  aValue = val;
  aPreventCachingOfSandwich =
              (unitType == nsIDOMSVGLength::SVG_LENGTHTYPE_PERCENTAGE ||
               unitType == nsIDOMSVGLength::SVG_LENGTHTYPE_EMS ||
               unitType == nsIDOMSVGLength::SVG_LENGTHTYPE_EXS);

  return NS_OK;
}

nsSMILValue
nsSVGLength2::SMILLength::GetBaseValue() const
{
  nsSMILValue val(nsSMILFloatType::Singleton());
  val.mU.mDouble = mVal->GetBaseValue(mSVGElement);
  return val;
}

void
nsSVGLength2::SMILLength::ClearAnimValue()
{
  if (mVal->mIsAnimated) {
    mVal->mIsAnimated = false;
    mVal->mAnimVal = mVal->mBaseVal;
    mSVGElement->DidAnimateLength(mVal->mAttrEnum);
  }  
}

nsresult
nsSVGLength2::SMILLength::SetAnimValue(const nsSMILValue& aValue)
{
  NS_ASSERTION(aValue.mType == nsSMILFloatType::Singleton(),
    "Unexpected type to assign animated value");
  if (aValue.mType == nsSMILFloatType::Singleton()) {
    mVal->SetAnimValue(float(aValue.mU.mDouble), mSVGElement);
  }
  return NS_OK;
}
