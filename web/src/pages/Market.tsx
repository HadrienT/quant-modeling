import { useEffect, useMemo, useState } from "react";
import { getCleanedIVSurface, getIVSurface, getLocalVolSurface, getMarketHistory, getRatesCurve, getTickers } from "../api/client";
import { PriceTab } from "./market/PriceTab";
import { RatesTab } from "./market/RatesTab";
import { VolTab, type VolSubTab } from "./market/VolTab";
import { CurvePoint, PricePoint, TimeRange } from "./market/types";

type MarketTab = "price" | "vol" | "rates";

export default function Market() {
	const [equity, setEquity] = useState("AAPL");
	const [debouncedEquity, setDebouncedEquity] = useState("AAPL");
	const [volEquity, setVolEquity] = useState("AAPL");
	const [curve, setCurve] = useState("Treasury");
	const [surface, setSurface] = useState("Mid vol");
	const [ratesCurveView, setRatesCurveView] = useState<"zero" | "forward">("zero");
	const [ratesFixedPeriodYears, setRatesFixedPeriodYears] = useState(0.5);
	const [range, setRange] = useState<TimeRange>("6M");
	const [tab, setTab] = useState<MarketTab>("price");
	const [priceSeries, setPriceSeries] = useState<PricePoint[]>([]);
	const [priceLoading, setPriceLoading] = useState(false);
	const [priceError, setPriceError] = useState<string | null>(null);
	const [yAxisPadding, setYAxisPadding] = useState(10);
	const [tickers, setTickers] = useState<string[]>([]);
	const [tickersLoading, setTickersLoading] = useState(true);
	const [ivStrikes, setIvStrikes] = useState<number[]>([]);
	const [ivMaturities, setIvMaturities] = useState<number[]>([]);
	const [ivValues, setIvValues] = useState<Array<Array<number | null>>>([]);
	const [ivLoading, setIvLoading] = useState(false);
	const [ivError, setIvError] = useState<string | null>(null);
	const [volSubTab, setVolSubTab] = useState<VolSubTab>("raw");
	// Cleaned IV surface
	const [cleanStrikes, setCleanStrikes] = useState<number[]>([]);
	const [cleanMaturities, setCleanMaturities] = useState<number[]>([]);
	const [cleanValues, setCleanValues] = useState<Array<Array<number | null>>>([]);
	const [cleanLoading, setCleanLoading] = useState(false);
	const [cleanError, setCleanError] = useState<string | null>(null);
	// Local vol surface
	const [lvStrikes, setLvStrikes] = useState<number[]>([]);
	const [lvMaturities, setLvMaturities] = useState<number[]>([]);
	const [lvValues, setLvValues] = useState<Array<Array<number | null>>>([]);
	const [lvLoading, setLvLoading] = useState(false);
	const [lvError, setLvError] = useState<string | null>(null);
	const [zeroCurve, setZeroCurve] = useState<CurvePoint[]>([]);
	const [ratesLoading, setRatesLoading] = useState(false);
	const [ratesError, setRatesError] = useState<string | null>(null);

	useEffect(() => {
		getTickers()
			.then((data) => {
				setTickers(data.tickers);
			})
			.catch((err) => {
				console.error("Failed to load tickers:", err);
			})
			.finally(() => {
				setTickersLoading(false);
			});
	}, []);

	// Debounce equity input to avoid excessive API calls while typing
	useEffect(() => {
		const timer = setTimeout(() => {
			setDebouncedEquity(equity);
		}, 500);
		return () => clearTimeout(timer);
	}, [equity]);

	useEffect(() => {
		let active = true;
		setPriceError(null);

		if (!debouncedEquity.trim()) {
			setPriceSeries([]);
			setPriceLoading(false);
			return () => {
				active = false;
			};
		}

		if (!tickersLoading && !tickers.includes(debouncedEquity)) {
			setPriceSeries([]);
			setPriceError("Unknown ticker. Select a valid symbol from the list.");
			setPriceLoading(false);
			return () => {
				active = false;
			};
		}

		setPriceLoading(true);
		getMarketHistory(debouncedEquity, range)
			.then((data) => {
				if (!active) return;
				setPriceSeries(data.points.map((point) => ({ date: point.date, price: point.close })));
			})
			.catch((err) => {
				if (!active) return;
				const message = err instanceof Error ? err.message : "Failed to load prices";
				setPriceError(message);
			})
			.finally(() => {
				if (!active) return;
				setPriceLoading(false);
			});
		return () => {
			active = false;
		};
	}, [debouncedEquity, range, tickers, tickersLoading]);

	useEffect(() => {
		let active = true;
		setIvError(null);

		// Only fetch IV surface when the vol tab is active
		if (tab !== "vol") {
			return () => {
				active = false;
			};
		}

		if (!volEquity.trim()) {
			setIvStrikes([]);
			setIvMaturities([]);
			setIvValues([]);
			setIvLoading(false);
			return () => {
				active = false;
			};
		}

		if (!tickersLoading && !tickers.includes(volEquity)) {
			setIvStrikes([]);
			setIvMaturities([]);
			setIvValues([]);
			setIvError("Unknown ticker. Select a valid symbol from the list.");
			setIvLoading(false);
			return () => {
				active = false;
			};
		}

		const surfaceKey = surface === "Bid vol" ? "bid" : surface === "Ask vol" ? "ask" : "mid";
		setIvLoading(true);
		getIVSurface(volEquity, surfaceKey)
			.then((data) => {
				if (!active) return;
				setIvStrikes(data.strikes);
				setIvMaturities(data.maturities);
				setIvValues(data.values);
			})
			.catch((err) => {
				if (!active) return;
				const message = err instanceof Error ? err.message : "Failed to load IV surface";
				setIvError(message);
			})
			.finally(() => {
				if (!active) return;
				setIvLoading(false);
			});
		return () => {
			active = false;
		};
	}, [volEquity, surface, tickers, tickersLoading, tab]);

	// Fetch cleaned IV surface when sub-tab is "cleaned"
	useEffect(() => {
		let active = true;
		setCleanError(null);

		if (tab !== "vol" || volSubTab !== "cleaned") {
			return () => { active = false; };
		}

		if (!volEquity.trim() || (!tickersLoading && !tickers.includes(volEquity))) {
			setCleanStrikes([]);
			setCleanMaturities([]);
			setCleanValues([]);
			if (volEquity.trim() && !tickersLoading) setCleanError("Unknown ticker.");
			return () => { active = false; };
		}

		setCleanLoading(true);
		getCleanedIVSurface(volEquity)
			.then((data) => {
				if (!active) return;
				setCleanStrikes(data.strikes);
				setCleanMaturities(data.maturities);
				setCleanValues(data.values);
			})
			.catch((err) => {
				if (!active) return;
				setCleanError(err instanceof Error ? err.message : "Failed to load cleaned IV surface");
			})
			.finally(() => {
				if (!active) return;
				setCleanLoading(false);
			});
		return () => { active = false; };
	}, [volEquity, tickers, tickersLoading, tab, volSubTab]);

	// Fetch local vol surface when sub-tab is "local-vol"
	useEffect(() => {
		let active = true;
		setLvError(null);

		if (tab !== "vol" || volSubTab !== "local-vol") {
			return () => { active = false; };
		}

		if (!volEquity.trim() || (!tickersLoading && !tickers.includes(volEquity))) {
			setLvStrikes([]);
			setLvMaturities([]);
			setLvValues([]);
			if (volEquity.trim() && !tickersLoading) setLvError("Unknown ticker.");
			return () => { active = false; };
		}

		setLvLoading(true);
		getLocalVolSurface(volEquity)
			.then((data) => {
				if (!active) return;
				setLvStrikes(data.strikes);
				setLvMaturities(data.maturities);
				setLvValues(data.values);
			})
			.catch((err) => {
				if (!active) return;
				setLvError(err instanceof Error ? err.message : "Failed to load local vol surface");
			})
			.finally(() => {
				if (!active) return;
				setLvLoading(false);
			});
		return () => { active = false; };
	}, [volEquity, tickers, tickersLoading, tab, volSubTab]);

	useEffect(() => {
		let active = true;
		setRatesError(null);

		if (tab !== "rates") {
			return () => {
				active = false;
			};
		}

		setRatesLoading(true);
		getRatesCurve(curve as "Treasury" | "SOFR" | "FedFunds", ratesCurveView, ratesFixedPeriodYears)
			.then((data) => {
				if (!active) return;
				setZeroCurve(data.zero);
			})
			.catch((err) => {
				if (!active) return;
				const message = err instanceof Error ? err.message : "Failed to load rates curve";
				setRatesError(message);
			})
			.finally(() => {
				if (!active) return;
				setRatesLoading(false);
			});

		return () => {
			active = false;
		};
	}, [curve, ratesCurveView, ratesFixedPeriodYears, tab]);

	const visibleSeries = priceSeries;
	const latest = visibleSeries.length > 0 ? visibleSeries[visibleSeries.length - 1] : undefined;
	const previous = visibleSeries.length > 1 ? visibleSeries[visibleSeries.length - 2] : latest;
	const change = latest && previous ? latest.price - previous.price : 0;
	const changePct = latest && previous ? (change / previous.price) * 100 : 0;

	const yAxisRange = useMemo(() => {
		if (visibleSeries.length === 0) return undefined;
		const prices = visibleSeries.map((p) => p.price);
		const minPrice = Math.min(...prices);
		const maxPrice = Math.max(...prices);
		const padding = (maxPrice - minPrice) * (yAxisPadding / 100);
		return [minPrice - padding, maxPrice + 0.2 * padding];
	}, [visibleSeries, yAxisPadding]);

	return (
		<>
			<section className="hero">
				<div>
					<h1>Market surfaces and curves.</h1>
					<p>
						Track the implied vol landscape and the term structure that feeds pricing. Pick a curve or surface here, then
						use it in pricing.
					</p>
				</div>
			</section>

			<section className="card" style={{ marginBottom: 24 }}>
				<h2>Desk views</h2>
				<p className="price-muted">Switch between pricing, volatility, and curve analytics.</p>
				<div style={{ display: "flex", gap: 10, flexWrap: "wrap", marginTop: 16 }}>
					<button
						className={`button ${tab === "price" ? "primary" : "secondary"}`}
						onClick={() => setTab("price")}
					>
						Price tape
					</button>
					<button
						className={`button ${tab === "vol" ? "primary" : "secondary"}`}
						onClick={() => setTab("vol")}
					>
						Implied vol
					</button>
					<button
						className={`button ${tab === "rates" ? "primary" : "secondary"}`}
						onClick={() => setTab("rates")}
					>
						Rate curves
					</button>
				</div>
			</section>

			{tab === "price" && (
				<PriceTab
					equity={equity}
					onEquityChange={setEquity}
					tickers={tickers}
					tickersLoading={tickersLoading}
					debouncedEquity={debouncedEquity}
					range={range}
					onRangeChange={setRange}
					yAxisPadding={yAxisPadding}
					onPaddingChange={setYAxisPadding}
					priceError={priceError}
					priceLoading={priceLoading}
					latest={latest}
					previous={previous}
					change={change}
					changePct={changePct}
					visibleSeries={visibleSeries}
					yAxisRange={yAxisRange}
				/>
			)}

			{tab === "vol" && (
				<VolTab
					equity={volEquity}
					onEquityChange={setVolEquity}
					tickers={tickers}
					tickersLoading={tickersLoading}
					surface={surface}
					onSurfaceChange={setSurface}
					subTab={volSubTab}
					onSubTabChange={setVolSubTab}
					raw={{ strikes: ivStrikes, maturities: ivMaturities, values: ivValues, loading: ivLoading, error: ivError }}
					cleaned={{ strikes: cleanStrikes, maturities: cleanMaturities, values: cleanValues, loading: cleanLoading, error: cleanError }}
					localVol={{ strikes: lvStrikes, maturities: lvMaturities, values: lvValues, loading: lvLoading, error: lvError }}
				/>
			)}

			{tab === "rates" && (
				<RatesTab
					curve={curve}
					onCurveChange={setCurve}
					zeroCurve={zeroCurve}
					curveView={ratesCurveView}
					onCurveViewChange={setRatesCurveView}
					fixedPeriodYears={ratesFixedPeriodYears}
					onFixedPeriodYearsChange={setRatesFixedPeriodYears}
					loading={ratesLoading}
					error={ratesError}
				/>
			)}
		</>
	);
}
