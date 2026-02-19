
#ifndef INSTRUMENT_BASE_HPP
#define INSTRUMENT_BASE_HPP
#include <quantModeling/core/types.hpp>

#include <vector>
namespace quantModeling
{
  struct VanillaOption; // fwd
  struct AsianOption;
  struct EquityFuture;
  struct ZeroCouponBond;
  struct FixedRateBond;

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