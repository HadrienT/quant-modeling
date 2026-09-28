import { Link } from "@tanstack/react-router";
import { SITE } from "@/shared/config";
import { ContactLink, LegalPage, Section } from "./LegalPage";

/** Mentions légales: what French law (LCEN art. 6-III) asks a site to state. */
export default function LegalNoticePage() {
	return (
		<LegalPage title="Legal notice">
			<Section title="Publisher">
				<p>
					{SITE.name} ({SITE.url.replace("https://", "")}) is a personal,
					non-commercial project published by {SITE.publisher}, a private
					individual. Publication director: {SITE.publisher}.
				</p>
				<p>
					Contact: <ContactLink />. A postal address is given on request at the
					same address.
				</p>
			</Section>

			<Section title="Hosting">
				<p>
					The site is self-hosted: the application, its database and its logs
					run on a server operated by the publisher, in France. Contact the host
					at <ContactLink />.
				</p>
				<p>
					Traffic reaches that server through a tunnel operated by Cloudflare,
					Inc., 101 Townsend St, San Francisco, CA 94107, USA (cloudflare.com),
					which terminates HTTPS and filters attacks. No port of the server is
					open to the internet.
				</p>
			</Section>

			<Section title="Intellectual property">
				<p>
					The source code is published under the MIT licence on{" "}
					<a
						className="text-accent hover:underline"
						href={SITE.repository}
						target="_blank"
						rel="noreferrer"
					>
						GitHub
					</a>
					. The texts of the site are the publisher's; the papers and books
					cited in the product documentation belong to their authors and
					publishers.
				</p>
			</Section>

			<Section title="Market data">
				<p>
					Prices, option chains, dividend yields and rates come from public
					sources: the Federal Reserve Bank of St. Louis (FRED), the European
					Central Bank, the Bank of England and Yahoo Finance. They remain the
					property of those providers, are shown for illustration, and may be
					delayed, incomplete or wrong. They must not be used to trade.
				</p>
			</Section>

			<Section title="See also">
				<p>
					How the site handles personal data is in the{" "}
					<Link to="/privacy" className="text-accent hover:underline">
						privacy policy
					</Link>
					; what its numbers are and are not, in the{" "}
					<Link to="/terms" className="text-accent hover:underline">
						terms and disclaimer
					</Link>
					.
				</p>
			</Section>
		</LegalPage>
	);
}
