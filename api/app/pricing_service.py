from typing import Dict

import quantmodeling as qm

from .schemas import AsianAverageType, AsianRequest, EngineType, PricingResponse, VanillaRequest


def _pricing_response_from_dict(result: Dict) -> PricingResponse:
    return PricingResponse(
        npv=result["npv"],
        greeks=result["greeks"],
        diagnostics=result.get("diagnostics", ""),
        mc_std_error=result.get("mc_std_error", 0.0),
    )


def price_vanilla(req: VanillaRequest) -> PricingResponse:
    input_data = qm.VanillaBSInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.is_call = req.is_call
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_epsilon = req.mc_epsilon

    if req.engine == EngineType.mc:
        result = qm.price_vanilla_bs_mc(input_data)
    else:
        result = qm.price_vanilla_bs_analytic(input_data)

    return _pricing_response_from_dict(result)


def price_asian(req: AsianRequest) -> PricingResponse:
    input_data = qm.AsianBSInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.is_call = req.is_call
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_epsilon = req.mc_epsilon

    if req.average_type == AsianAverageType.geometric:
        input_data.average_type = qm.AsianAverageType.Geometric
    else:
        input_data.average_type = qm.AsianAverageType.Arithmetic

    if req.engine == EngineType.mc:
        result = qm.price_asian_bs_mc(input_data)
    else:
        result = qm.price_asian_bs_analytic(input_data)

    return _pricing_response_from_dict(result)
