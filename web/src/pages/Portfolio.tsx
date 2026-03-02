import { useCallback, useEffect, useState } from "react";
import type {
	BatchPriceResponse,
	Portfolio,
	PortfolioRiskSummary,
	PortfolioSummary,
	Position,
	StressBump,
	StressResult,
	VaRResult,
} from "../api/client";
import {
	addPosition,
	computeVaR,
	createPortfolio,
	deletePortfolio,
	getPortfolio,
	listPortfolios,
	pricePortfolio,
	removePosition,
	stressTestPortfolio,
	updatePosition,
} from "../api/client";
import { useAuth } from "../auth/AuthContext";
import {
	localAddPosition,
	localCreatePortfolio,
	localDeletePortfolio,
	localGetPortfolio,
	localListPortfolios,
	localRemovePosition,
	localUpdatePosition,
} from "../auth/localPortfolios";
import AddPositionForm from "./portfolio/AddPositionForm";
import PositionDetail from "./portfolio/PositionDetail";
import PositionTable from "./portfolio/PositionTable";
import RiskPanel from "./portfolio/RiskPanel";

const DEFAULT_BUMPS: StressBump[] = [
	{ name: "Spot −10 %", spot_shift: -0.10, vol_shift: 0, rate_shift: 0 },
	{ name: "Spot +10 %", spot_shift: 0.10, vol_shift: 0, rate_shift: 0 },
	{ name: "Vol +5 pts", spot_shift: 0, vol_shift: 0.05, rate_shift: 0 },
	{ name: "Vol −5 pts", spot_shift: 0, vol_shift: -0.05, rate_shift: 0 },
	{ name: "Rate +100 bps", spot_shift: 0, vol_shift: 0, rate_shift: 0.01 },
	{ name: "Rate −100 bps", spot_shift: 0, vol_shift: 0, rate_shift: -0.01 },
	{ name: "Crash (spot −20 %, vol +10)", spot_shift: -0.20, vol_shift: 0.10, rate_shift: -0.005 },
];

export default function PortfolioPage() {
	const { username } = useAuth();
	const isLoggedIn = !!username;

	// ── Portfolio list ────────────────────────────────────
	const [summaries, setSummaries] = useState<PortfolioSummary[]>([]);
	const [activeId, setActiveId] = useState<string | null>(null);
	const [portfolio, setPortfolio] = useState<Portfolio | null>(null);

	// ── UI state ────────────────────────────────────────
	const [showAddForm, setShowAddForm] = useState(false);
	const [selectedPosition, setSelectedPosition] = useState<Position | null>(null);
	const [loading, setLoading] = useState(false);
	const [riskLoading, setRiskLoading] = useState(false);
	const [error, setError] = useState<string | null>(null);
	const [newName, setNewName] = useState("");

	// ── Risk state ──────────────────────────────────────
	const [risk, setRisk] = useState<PortfolioRiskSummary | null>(null);
	const [varResult, setVarResult] = useState<VaRResult | null>(null);
	const [stressResults, setStressResults] = useState<StressResult[]>([]);

	// ── Refresh list ─────────────────────────────────────
	const refresh = useCallback(async () => {
		try {
			if (isLoggedIn) {
				setSummaries(await listPortfolios());
			} else {
				setSummaries(localListPortfolios());
			}
		} catch {
			/* ignore */
		}
	}, [isLoggedIn]);

	// Reset when auth state changes
	useEffect(() => {
		setActiveId(null);
		setPortfolio(null);
		setRisk(null);
		setVarResult(null);
		setStressResults([]);
		refresh();
	}, [isLoggedIn, refresh]);

	// ── Load active portfolio ────────────────────────────
	useEffect(() => {
		if (!activeId) {
			setPortfolio(null);
			setRisk(null);
			setVarResult(null);
			setStressResults([]);
			return;
		}
		(async () => {
			try {
				if (isLoggedIn) {
					setPortfolio(await getPortfolio(activeId));
				} else {
					setPortfolio(localGetPortfolio(activeId));
				}
			} catch {
				setError("Failed to load portfolio");
			}
		})();
	}, [activeId, isLoggedIn]);

	// ── Create portfolio ─────────────────────────────────
	const handleCreate = async () => {
		const name = newName.trim() || "Untitled Portfolio";
		setLoading(true);
		try {
			let pf: Portfolio;
			if (isLoggedIn) {
				pf = await createPortfolio(name);
			} else {
				pf = localCreatePortfolio(name);
			}
			setNewName("");
			await refresh();
			setActiveId(pf.id);
		} catch {
			setError("Failed to create portfolio");
		} finally {
			setLoading(false);
		}
	};

	// ── Delete portfolio ─────────────────────────────────
	const handleDelete = async () => {
		if (!activeId) return;
		if (!confirm("Delete this portfolio?")) return;
		try {
			if (isLoggedIn) {
				await deletePortfolio(activeId);
			} else {
				localDeletePortfolio(activeId);
			}
			setActiveId(null);
			await refresh();
		} catch {
			setError("Failed to delete portfolio");
		}
	};

	// ── Add position ─────────────────────────────────────
	const handleAddPosition = async (pos: Position) => {
		if (!activeId) return;
		setLoading(true);
		try {
			let pf: Portfolio;
			if (isLoggedIn) {
				pf = await addPosition(activeId, pos);
			} else {
				pf = localAddPosition(activeId, pos);
			}
			setPortfolio(pf);
			setShowAddForm(false);
			await refresh();
		} catch {
			setError("Failed to add position");
		} finally {
			setLoading(false);
		}
	};

	// ── Remove position ──────────────────────────────────
	const handleRemovePosition = async (posId: string) => {
		if (!activeId) return;
		try {
			let pf: Portfolio;
			if (isLoggedIn) {
				pf = await removePosition(activeId, posId);
			} else {
				pf = localRemovePosition(activeId, posId);
			}
			setPortfolio(pf);
			if (selectedPosition?.id === posId) setSelectedPosition(null);
			await refresh();
		} catch {
			setError("Failed to remove position");
		}
	};

	// ── Update position (edit) ──────────────────────────
	const handleUpdatePosition = async (pos: Position) => {
		if (!activeId) return;
		setLoading(true);
		try {
			let pf: Portfolio;
			if (isLoggedIn) {
				pf = await updatePosition(activeId, pos);
			} else {
				pf = localUpdatePosition(activeId, pos);
			}
			setPortfolio(pf);
			setSelectedPosition(null);
			await refresh();
		} catch {
			setError("Failed to update position");
		} finally {
			setLoading(false);
		}
	};

	// ── Price all ────────────────────────────────────────
	const handlePrice = async () => {
		if (!activeId) return;
		setLoading(true);
		setError(null);
		try {
			if (isLoggedIn) {
				const res: BatchPriceResponse = await pricePortfolio(activeId);
				setPortfolio(res.portfolio);
				setRisk(res.risk_summary);
			} else {
				// For anonymous: send the portfolio to the pricing endpoint
				// We need to save the portfolio locally first, then call price via API
				// But API requires auth. So use local pricing which calls the single-price endpoint
				// Actually: pricing requires auth behind require_user in the router.
				// For anonymous, we cannot price server-side. Show a message.
				setError("Log in to price portfolios with the C++ engine.");
				setLoading(false);
				return;
			}
			await refresh();
		} catch (e) {
			setError("Pricing failed: " + (e instanceof Error ? e.message : ""));
		} finally {
			setLoading(false);
		}
	};

	// ── VaR ──────────────────────────────────────────────
	const handleVaR = async () => {
		if (!activeId) return;
		if (!isLoggedIn) { setError("Log in to compute VaR."); return; }
		setRiskLoading(true);
		try {
			const res = await computeVaR(activeId);
			setVarResult(res);
		} catch {
			setError("VaR computation failed");
		} finally {
			setRiskLoading(false);
		}
	};

	// ── Stress ───────────────────────────────────────────
	const handleStress = async () => {
		if (!activeId) return;
		if (!isLoggedIn) { setError("Log in to run stress tests."); return; }
		setRiskLoading(true);
		try {
			const res = await stressTestPortfolio(activeId, DEFAULT_BUMPS);
			setStressResults(res);
		} catch {
			setError("Stress test failed");
		} finally {
			setRiskLoading(false);
		}
	};

	return (
		<>
			<section className="hero">
				<div>
					<h1>Portfolio desk.</h1>
					<p>
						Create portfolios, add positions across all asset classes, price with the C++ engine, and
						monitor risk with Greeks, VaR, and stress tests.
					</p>
				</div>
			</section>

			{error && (
				<div className="error-banner" onClick={() => setError(null)}>
					{error}
				</div>
			)}

			<section className="portfolio-layout">
				{/* ── Sidebar ──────────────────────────────── */}
				<aside className="pf-sidebar card">
					<h3>Portfolios</h3>
					<div className="pf-create-row">
						<input
							type="text"
							placeholder="New portfolio name…"
							value={newName}
							onChange={(e) => setNewName(e.target.value)}
							onKeyDown={(e) => e.key === "Enter" && handleCreate()}
						/>
						<button type="button" className="btn-primary btn-sm" onClick={handleCreate} disabled={loading}>
							+
						</button>
					</div>
					<ul className="pf-list">
						{summaries.map((s) => (
							<li key={s.id} className={`pf-list-item${activeId === s.id ? " active" : ""}`}>
								<button type="button" onClick={() => setActiveId(s.id)}>
									<span className="pf-name">{s.name}</span>
									<span className="pf-meta">
										{s.n_positions} pos · {s.total_value !== 0 ? s.total_value.toLocaleString(undefined, { maximumFractionDigits: 2 }) : "—"}
									</span>
								</button>
							</li>
						))}
						{summaries.length === 0 && <li className="pf-empty">No portfolios yet</li>}
					</ul>
				</aside>

				{/* ── Main area ──────────────────────────── */}
				<div className="pf-main">
					{!portfolio ? (
						<div className="pf-placeholder card">
							<p>Select or create a portfolio to begin.</p>
						</div>
					) : (
						<>
							{/* ── Header bar ── */}
							<div className="pf-header card">
								<div className="pf-header-left">
									<h2>{portfolio.name}</h2>
									<span className="pf-meta-text">
										{portfolio.positions.length} positions ·
										Updated {new Date(portfolio.updated_at).toLocaleString()}
									</span>
								</div>
								<div className="pf-header-actions">
									<button type="button" className="btn-primary" onClick={() => setShowAddForm(true)}>
										Add Position
									</button>
									<button
										type="button"
										className="btn-primary"
										onClick={handlePrice}
										disabled={loading || portfolio.positions.length === 0}
									>
										{loading ? "Pricing…" : "Price All"}
									</button>
									<button type="button" className="btn-danger btn-sm" onClick={handleDelete}>
										Delete
									</button>
								</div>
							</div>

							{/* ── Positions table ── */}
							<div className="card">
								<PositionTable
									positions={portfolio.positions}
									onRemove={handleRemovePosition}
									onSelect={setSelectedPosition}
								/>
							</div>

							{/* ── Risk panel ── */}
							<RiskPanel
								risk={risk}
								var_={varResult}
								stress={stressResults}
								onRunVar={handleVaR}
								onRunStress={handleStress}
								loading={riskLoading}
							/>
						</>
					)}
				</div>
			</section>

			{/* ── Add position overlay ── */}
			{showAddForm && (
				<AddPositionForm onAdd={handleAddPosition} onCancel={() => setShowAddForm(false)} />
			)}

			{/* ── Position detail / edit overlay ── */}
			{selectedPosition && (
				<PositionDetail
					position={selectedPosition}
					onClose={() => setSelectedPosition(null)}
					onSave={handleUpdatePosition}
				/>
			)}
		</>
	);
}
