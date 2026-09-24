import type { components } from "./schema.gen";

/** Convenience aliases over the generated component schemas. */
export type Schemas = components["schemas"];

export type PricingResponse = Schemas["PricingResponse"];
export type Greeks = Schemas["Greeks"];
export type BondAnalytics = Schemas["BondAnalytics"];

export type CleanedIVSurfaceResponse = Schemas["CleanedIVSurfaceResponse"];
export type DeltaBucketRow = Schemas["DeltaBucketRow"];
export type DeltaSurfaceResponse = Schemas["DeltaSurfaceResponse"];
export type LocalVolSurfaceResponse = Schemas["LocalVolSurfaceResponse"];
export type RatesOverviewResponse = Schemas["RatesOverviewResponse"];
export type RatesHistoryResponse = Schemas["RatesHistoryResponse"];
export type GovernmentCurveResponse = Schemas["GovernmentCurveResponse"];
export type BenchmarkRate = Schemas["BenchmarkRate"];
export type RateCurrency = RatesOverviewResponse["currency"];
export type MarketHistoryResponse = Schemas["MarketHistoryResponse"];
export type MarketInfo = Schemas["MarketInfo"];
export type MarketId = MarketInfo["id"];
export type TickerInfo = Schemas["TickerInfo"];

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

export type SimulationModel = Schemas["SimulationModel"];
export type BSPathRequest = Schemas["BSPathRequest"];
export type SABRPathRequest = Schemas["SABRPathRequest"];
export type SimulationPathsResponse = Schemas["SimulationPathsResponse"];
export type SimulationCalibrateRequest = Schemas["SimulationCalibrateRequest"];
export type SimulationCalibrateResponse =
	Schemas["SimulationCalibrateResponse"];
export type FxCurrency = Schemas["FxOverviewResponse"]["base"];
export type FxOverviewResponse = Schemas["FxOverviewResponse"];
export type FxHistoryResponse = Schemas["FxHistoryResponse"];
export type FxCorrelationResponse = Schemas["FxCorrelationResponse"];

// Portfolio ledger and its valuation (MTM, daily P&L).
export type Instrument = Schemas["Instrument"];
export type EquitySpec = Schemas["EquitySpec"];
export type DerivativeSpec = Schemas["DerivativeSpec"];
export type Trade = Schemas["Trade"];
export type PortfolioCurrency = Portfolio["base_currency"];
export type PortfolioSnapshot = Schemas["SnapshotResponse"];
export type PositionMark = Schemas["PositionMark"];
export type MarkInput = Schemas["InputView"];
export type PortfolioHistory = Schemas["HistoryResponse"];
export type PnlWindow = NonNullable<Schemas["HistoryRequest"]["window"]>;
export type TickerClose = Schemas["CloseResponse"];
