
#ifndef INSTRUMENT_BASE_HPP
#define INSTRUMENT_BASE_HPP
#include <quantModeling/core/types.hpp>

#include <vector>
namespace quantModeling
{
  struct VanillaOption; // fwd
  struct AsianOption;
  struct BarrierOption;
  struct DigitalOption;
  struct LookbackOption;
  struct BasketOption;
  struct EquityFuture;
  struct ZeroCouponBond;
  struct FixedRateBond;
  struct BondOption;
  struct CapFloor;
  struct AutocallNote;
  struct MountainOption;
  struct Caplet;
  struct VarianceSwap;
  struct VolatilitySwap;
  struct DispersionSwap;
  struct FXForward;
  struct FXOption;
  struct CommodityForward;
  struct CommodityOption;
  struct WorstOfOption;
  struct BestOfOption;

  enum class ExerciseType
  {
    European,
    American
  };

  struct IExercise
  {
    virtual ~IExercise() = default;
    virtual ExerciseType type() const = 0;
    virtual const std::vector<Time> &dates() const = 0; // year-fractions
  };

  struct EuropeanExercise final : IExercise
  {
    std::vector<Time> d_;
    explicit EuropeanExercise(Time maturity) : d_{maturity} {}

    ExerciseType type() const override { return ExerciseType::European; }
    const std::vector<Time> &dates() const override { return d_; }
  };

  struct AmericanExercise final : IExercise
  {
    std::vector<Time> d_;
    explicit AmericanExercise(Time maturity) : d_{maturity} {}

    ExerciseType type() const override { return ExerciseType::American; }
    const std::vector<Time> &dates() const override { return d_; }
  };

  struct IInstrumentVisitor
  {
    virtual ~IInstrumentVisitor() = default;
    virtual void visit(const VanillaOption &) = 0;
    virtual void visit(const AsianOption &) = 0;
    virtual void visit(const BarrierOption &) = 0;
    virtual void visit(const DigitalOption &) = 0;
    virtual void visit(const LookbackOption &) { throw UnsupportedInstrument("Lookback option is not supported by this engine."); }
    virtual void visit(const BasketOption &) { throw UnsupportedInstrument("Basket option is not supported by this engine."); }
    virtual void visit(const BondOption &) { throw UnsupportedInstrument("Bond option is not supported by this engine."); }
    virtual void visit(const CapFloor &) { throw UnsupportedInstrument("Cap/floor is not supported by this engine."); }
    virtual void visit(const AutocallNote &) { throw UnsupportedInstrument("Autocall note is not supported by this engine."); }
    virtual void visit(const MountainOption &) { throw UnsupportedInstrument("Mountain option is not supported by this engine."); }
    virtual void visit(const Caplet &) { throw UnsupportedInstrument("Caplet is not supported by this engine."); }
    virtual void visit(const VarianceSwap &) { throw UnsupportedInstrument("Variance swap is not supported by this engine."); }
    virtual void visit(const VolatilitySwap &) { throw UnsupportedInstrument("Volatility swap is not supported by this engine."); }
    virtual void visit(const DispersionSwap &) { throw UnsupportedInstrument("Dispersion swap is not supported by this engine."); }
    virtual void visit(const FXForward &) { throw UnsupportedInstrument("FX forward is not supported by this engine."); }
    virtual void visit(const FXOption &) { throw UnsupportedInstrument("FX option is not supported by this engine."); }
    virtual void visit(const CommodityForward &) { throw UnsupportedInstrument("Commodity forward is not supported by this engine."); }
    virtual void visit(const CommodityOption &) { throw UnsupportedInstrument("Commodity option is not supported by this engine."); }
    virtual void visit(const WorstOfOption &) { throw UnsupportedInstrument("Worst-of option is not supported by this engine."); }
    virtual void visit(const BestOfOption &) { throw UnsupportedInstrument("Best-of option is not supported by this engine."); }
    virtual void visit(const EquityFuture &) = 0;
    virtual void visit(const ZeroCouponBond &) = 0;
    virtual void visit(const FixedRateBond &) = 0;
  };

  struct Instrument
  {
    virtual ~Instrument() = default;
    virtual void accept(IInstrumentVisitor &v) const = 0;
  };

  enum class OptionType
  {
    Call,
    Put
  };

  struct IPayoff
  {
    virtual ~IPayoff() = default;
    virtual OptionType type() const = 0;
    virtual Real strike() const = 0;
    virtual Real operator()(Real spot) const = 0;
  };

} // namespace quantModeling
#endif
