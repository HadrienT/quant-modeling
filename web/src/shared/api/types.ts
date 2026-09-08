import type { components } from "./schema.gen";

/** Convenience aliases over the generated component schemas. */
export type Schemas = components["schemas"];

export type PricingResponse = Schemas["PricingResponse"];
export type Greeks = Schemas["Greeks"];
export type BondAnalytics = Schemas["BondAnalytics"];

export type IVSurfaceResponse = Schemas["IVSurfaceResponse"];
export type CleanedIVSurfaceResponse = Schemas["CleanedIVSurfaceResponse"];
export type LocalVolSurfaceResponse = Schemas["LocalVolSurfaceResponse"];
export type RatesCurveResponse = Schemas["RatesCurveResponse"];
export type MarketHistoryResponse = Schemas["MarketHistoryResponse"];

// FastAPI splits models used for both request and response bodies.
export type Portfolio = Schemas["Portfolio-Output"];
export type PortfolioInput = Schemas["Portfolio-Input"];
export type Position = Schemas["Position-Output"];
export type PositionInput = Schemas["Position-Input"];
export type PortfolioSummary = Schemas["PortfolioSummary"];
export type PortfolioRiskSummary = Schemas["PortfolioRiskSummary"];
export type BatchPriceResponse = Schemas["BatchPriceResponse"];
export type StressBump = Schemas["StressBump"];
export type StressResult = Schemas["StressResult"];
export type VaRResult = Schemas["VaRResult"];

export type BacktestRequest = Schemas["BacktestRequest"];
export type BacktestResponse = Schemas["BacktestResponse"];

export type AuthResponse = Schemas["AuthResponse"];
