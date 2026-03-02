/* ── category / instrument ────────────────────────── */
export type CategoryType = "vanilla" | "exotics" | "fixed-income" | "structured" | "volatility" | "fx" | "commodity";
export type InstrumentType = "option" | "future" | "bond";

/* ── model selection ─────────────────────────────── */
export type ModelType = "black-scholes" | "dupire-local-vol";
export type VolSourceType = "manual" | "real";

/* ── vanilla options ─────────────────────────────── */
export type EngineType = "analytic" | "mc" | "binomial" | "trinomial" | "pde";
export type ProductType = "vanilla" | "asian" | "american";
export type AverageType = "arithmetic" | "geometric";

/* ── exotic options ──────────────────────────────── */
export type ExoticProductType = "barrier" | "digital" | "lookback" | "basket" | "rainbow";
export type BarrierKind = "up-and-in" | "up-and-out" | "down-and-in" | "down-and-out";
export type DigitalPayoffType = "cash-or-nothing" | "asset-or-nothing";
export type LookbackStyle = "fixed-strike" | "floating-strike";
export type LookbackExtremum = "minimum" | "maximum";
export type RainbowKind = "worst-of" | "best-of";

/* ── structured products ─────────────────────────── */
export type StructuredProductType = "autocall" | "mountain";

/* ── volatility products ─────────────────────────── */
export type VolProductType = "variance-swap" | "volatility-swap" | "dispersion-swap";

/* ── FX products ─────────────────────────────────── */
export type FXProductType = "fx-forward" | "fx-option";

/* ── Commodity products ──────────────────────────── */
export type CommodityProductType = "commodity-forward" | "commodity-option";

/* ── fixed income ────────────────────────────────── */
export type CouponFrequency = "annual" | "semiannual" | "quarterly";
export type BondType = "zero-coupon" | "fixed-rate";
export type BondCurveSource = "manual" | "Treasury" | "SOFR" | "FedFunds";
export type CurvePoint = { time: number; df: number };

/* ── API response ────────────────────────────────── */
export type BondAnalytics = {
	macaulay_duration?: number | null;
	modified_duration?: number | null;
	convexity?: number | null;
	dv01?: number | null;
};

export type PricingResponse = {
	npv: number;
	greeks: {
		delta?: number | null;
		gamma?: number | null;
		vega?: number | null;
		theta?: number | null;
		rho?: number | null;
		delta_std_error?: number | null;
		gamma_std_error?: number | null;
		vega_std_error?: number | null;
		theta_std_error?: number | null;
		rho_std_error?: number | null;
	};
	bond_analytics?: BondAnalytics | null;
	diagnostics?: string | null;
	mc_std_error?: number | null;
};
