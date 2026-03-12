import { useState } from "react";
import { createPortal } from "react-dom";
import { NavLink, Navigate, Route, Routes } from "react-router-dom";
import { AuthProvider, useAuth } from "./auth/AuthContext";
import About from "./pages/About";
import BacktestPage from "./pages/Backtest";
import Price from "./pages/Price";
import Market from "./pages/Market";
import PortfolioPage from "./pages/Portfolio";
import Visualize from "./pages/Visualize";

function AuthButton() {
	const { username, login, register, logout, loading } = useAuth();
	const [showModal, setShowModal] = useState(false);
	const [isRegister, setIsRegister] = useState(false);
	const [user, setUser] = useState("");
	const [pass, setPass] = useState("");
	const [error, setError] = useState("");
	const [submitting, setSubmitting] = useState(false);

	if (loading) return null;

	if (username) {
		return (
			<div className="auth-logged">
				<span className="auth-user">{username}</span>
				<button type="button" className="btn-secondary btn-sm" onClick={logout}>
					Log out
				</button>
			</div>
		);
	}

	const handleSubmit = async () => {
		if (!user.trim() || !pass.trim()) return;
		setSubmitting(true);
		setError("");
		try {
			if (isRegister) {
				await register(user.trim(), pass);
			} else {
				await login(user.trim(), pass);
			}
			setShowModal(false);
			setUser("");
			setPass("");
		} catch (e) {
			setError(e instanceof Error ? e.message : "Auth failed");
		} finally {
			setSubmitting(false);
		}
	};

	return (
		<>
			<button type="button" className="btn-primary btn-sm" onClick={() => { setShowModal(true); setIsRegister(false); setError(""); }}>
				Log in
			</button>
			{showModal && createPortal(
				<div className="auth-overlay" onClick={() => setShowModal(false)}>
					<div className="auth-modal card" onClick={(e) => e.stopPropagation()}>
						<div className="auth-modal-header">
							<h3>{isRegister ? "Create account" : "Log in"}</h3>
							<button className="btn-icon" type="button" onClick={() => setShowModal(false)}>✕</button>
						</div>
						{error && <div className="auth-error">{error}</div>}
						<label className="field">
							Username
							<input
								type="text"
								value={user}
								onChange={(e) => setUser(e.target.value)}
								onKeyDown={(e) => e.key === "Enter" && handleSubmit()}
								autoFocus
							/>
						</label>
						<label className="field">
							Password
							<input
								type="password"
								value={pass}
								onChange={(e) => setPass(e.target.value)}
								onKeyDown={(e) => e.key === "Enter" && handleSubmit()}
							/>
						</label>
						<button type="button" className="btn-primary" onClick={handleSubmit} disabled={submitting} style={{ marginTop: 8, width: "100%" }}>
							{submitting ? "…" : isRegister ? "Register" : "Log in"}
						</button>
						<button
							type="button"
							className="auth-toggle"
							onClick={() => { setIsRegister(!isRegister); setError(""); }}
						>
							{isRegister ? "Already have an account? Log in" : "No account? Register"}
						</button>
					</div>
				</div>,
				document.body
			)}
		</>
	);
}

function AppInner() {
	return (
		<div className="app">
			<header className="nav">
				<div className="nav-inner">
					<button className="nav-brand" type="button" onClick={() => window.location.reload()}>
						<div className="nav-mark" />
						<div>
							<span className="nav-title">Quant studio</span>
							<span className="nav-subtitle">Derivatives modeling</span>
						</div>
					</button>
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
						<NavLink to="/portfolio" className="nav-link">
							Portfolio
						</NavLink>
						<NavLink to="/backtest" className="nav-link">
							Backtest
						</NavLink>
						<NavLink to="/about" className="nav-link">
							About
						</NavLink>
						<AuthButton />
					</nav>
				</div>
			</header>
			<main className="page">
				<Routes>
					<Route path="/" element={<Visualize />} />
					<Route path="/visualize" element={<Visualize />} />
					<Route path="/market" element={<Market />} />
					<Route path="/price" element={<Price />} />
					<Route path="/portfolio" element={<PortfolioPage />} />
					<Route path="/backtest" element={<BacktestPage />} />
					<Route path="/about" element={<About />} />
					<Route path="*" element={<Navigate to="/" replace />} />
				</Routes>
			</main>
			<footer className="build-footer" title={`Build ${import.meta.env.VITE_COMMIT_SHA || "dev"}`}>{import.meta.env.VITE_COMMIT_SHA || "dev"}</footer>
		</div>
	);
}

export default function App() {
	return (
		<AuthProvider>
			<AppInner />
		</AuthProvider>
	);
}
