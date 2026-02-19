import { useState } from "react";
import { NavLink, Navigate, Route, Routes } from "react-router-dom";
import About from "./pages/About";
import Price from "./pages/Price";
import Market from "./pages/Market";
import Visualize from "./pages/Visualize";
import { OptionLeg, PortfolioDraft } from "./components/PortfolioBuilder";

const baseLeg: OptionLeg = {
	id: "base",
	name: "Single Option",
	type: "call",
	side: "long",
	strike: 100,
	premium: 4,
	quantity: 1
};

const defaultDraft: PortfolioDraft = {
	name: "Leg",
	type: "call",
	side: "long",
	strike: 100,
	premium: 4,
	quantity: 1
};

export default function App() {
	const [singleLeg, setSingleLeg] = useState<OptionLeg>(baseLeg);
	const [portfolio, setPortfolio] = useState<OptionLeg[]>([
		{ ...baseLeg, id: "leg-1", name: "Starter Call" },
		{ ...baseLeg, id: "leg-2", name: "Hedge Put", type: "put", side: "long", strike: 90, premium: 2.4 }
	]);
	const [draft, setDraft] = useState<PortfolioDraft>(defaultDraft);
	const [focusAggregate, setFocusAggregate] = useState(false);
	const [spotMin, setSpotMin] = useState(60);
	const [spotMax, setSpotMax] = useState(140);

	return (
		<div className="app">
			<header className="nav">
				<div className="nav-inner">
					<div className="nav-brand">
						<div className="nav-mark" />
						<div>
							<span className="nav-title">Quant studio</span>
							<span className="nav-subtitle">Derivatives modeling</span>
						</div>
					</div>
					<nav className="nav-links">
						<NavLink to="/" end className="nav-link">
							Visualize
						</NavLink>
						<NavLink to="/market" className="nav-link">
							Market
						</NavLink>
						<NavLink to="/price" className="nav-link">
							Price
						</NavLink>
						<NavLink to="/about" className="nav-link">
							About
						</NavLink>
					</nav>
				</div>
			</header>
			<main className="page">
				<Routes>
					<Route
						path="/"
						element={
							<Visualize
								singleLeg={singleLeg}
								setSingleLeg={setSingleLeg}
								portfolio={portfolio}
								setPortfolio={setPortfolio}
								draft={draft}
								setDraft={setDraft}
								focusAggregate={focusAggregate}
								setFocusAggregate={setFocusAggregate}
								spotMin={spotMin}
								setSpotMin={setSpotMin}
								spotMax={spotMax}
								setSpotMax={setSpotMax}
							/>
						}
					/>
					<Route
						path="/visualize"
						element={
							<Visualize
								singleLeg={singleLeg}
								setSingleLeg={setSingleLeg}
								portfolio={portfolio}
								setPortfolio={setPortfolio}
								draft={draft}
								setDraft={setDraft}
								focusAggregate={focusAggregate}
								setFocusAggregate={setFocusAggregate}
								spotMin={spotMin}
								setSpotMin={setSpotMin}
								spotMax={spotMax}
								setSpotMax={setSpotMax}
							/>
						}
					/>
					<Route path="/market" element={<Market />} />
					<Route path="/price" element={<Price />} />
					<Route path="/about" element={<About />} />
					<Route path="*" element={<Navigate to="/" replace />} />
				</Routes>
			</main>
		</div>
	);
}
