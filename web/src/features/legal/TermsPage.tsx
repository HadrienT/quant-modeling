import { Link } from "@tanstack/react-router";
import { SITE } from "@/shared/config";
import { ContactLink, LegalPage, Section } from "./LegalPage";

/** Terms of use, and the financial disclaimer the pricing pages link to. */
export default function TermsPage() {
	return (
		<LegalPage title="Terms & disclaimer">
			<Section title="Not investment advice">
				<p className="text-ink">
					{SITE.name} is an educational and research tool. Nothing on it is
					investment advice, a recommendation, or an offer to buy or sell any
					financial instrument.
				</p>
				<p>
					The publisher is a private individual, not an investment firm or a
					financial adviser, and is not authorised or registered by the AMF or
					any other regulator.
				</p>
			</Section>

			<Section title="What the numbers are">
				<p>
					Prices, greeks, implied and local volatilities, P&amp;L, value at risk
					and backtests are the output of models. Each rests on assumptions (the
					model, its calibration, the market data it read) that the results
					pages state, and a Monte Carlo figure carries a sampling error. Past
					performance in a backtest says nothing about future returns.
				</p>
				<p>
					The site is provided as is, without warranty of accuracy, completeness
					or availability. To the extent the law allows, the publisher is not
					liable for any loss arising from its use. Check any figure
					independently before relying on it.
				</p>
			</Section>

			<Section title="Accounts">
				<p>
					Pricing, market data and the rest of the site work without an account.
					An account only keeps portfolios on the server.
				</p>
				<p>
					Signing in with Google is the durable way to hold one. A password
					account has no email attached, so a forgotten password cannot be
					recovered: treat it as disposable. Either kind can be exported and
					deleted at any time from the profile page.
				</p>
			</Section>

			<Section title="Fair use">
				<p>
					The scripting assistant and the Monte Carlo engines run on the
					publisher's own hardware, shared by every visitor. Do not automate
					bulk requests, scrape the market data, probe the site for
					vulnerabilities or try to get around its rate limits. The publisher
					may suspend an account or block traffic that does. A security issue is
					welcome at <ContactLink />.
				</p>
			</Section>

			<Section title="Availability">
				<p>
					This is a personal project: it may be down, change, or lose data
					without notice. Export anything you want to keep.
				</p>
			</Section>

			<Section title="Personal data">
				<p>
					See the{" "}
					<Link to="/privacy" className="text-accent hover:underline">
						privacy policy
					</Link>
					.
				</p>
			</Section>

			<Section title="Governing law">
				<p>
					These terms are governed by French law. The publisher may update them;
					the date above shows the latest version.
				</p>
			</Section>
		</LegalPage>
	);
}
