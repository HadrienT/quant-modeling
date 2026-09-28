import { Link } from "@tanstack/react-router";
import { SITE } from "@/shared/config";
import { ContactLink, LegalPage, Section } from "./LegalPage";
import { ProcessingTable } from "./ProcessingTable";

/** Privacy policy (GDPR art. 13): who, what, why, how long, and your rights. */
export default function PrivacyPage() {
	return (
		<LegalPage title="Privacy policy">
			<Section title="In short">
				<p>
					No advertising, no analytics, no third-party scripts, no tracking
					cookies. The site collects what it needs to run, keeps it on its own
					server, and sells or shares none of it.
				</p>
			</Section>

			<Section title="Controller">
				<p>
					{SITE.publisher}, publisher of {SITE.name}. Contact: <ContactLink />.
				</p>
			</Section>

			<Section title="What is processed, and for how long">
				<ProcessingTable />
			</Section>

			<Section title="Stored in your browser">
				<p>
					The site keeps a few items in your browser&apos;s local storage: your
					session token when you sign in, the light or dark theme, pricing
					presets, and portfolios made without an account. They never leave your
					browser, except the session token, which is sent to the API to
					identify you.
				</p>
				<p>
					The only cookie is set while you sign in with Google: it holds a
					random value that protects the sign-in against forgery, and lasts ten
					minutes. Both are strictly necessary to the service you ask for, so
					they need no consent and there is no cookie banner.
				</p>
			</Section>

			<Section title="Who else sees it">
				<p>
					<strong className="text-ink">Cloudflare</strong> carries the traffic
					to the server (see the{" "}
					<Link to="/legal" className="text-accent hover:underline">
						legal notice
					</Link>
					) and so sees IP addresses and requests in transit, under its own
					privacy policy; it is certified under the EU-US Data Privacy
					Framework. <strong className="text-ink">Google</strong> takes part
					only if you choose to sign in with it. Nobody else: the servers, the
					database, the logs and the assistant&apos;s language model are all run
					by the publisher.
				</p>
			</Section>

			<Section title="Your rights">
				<p>
					You may access, correct, export or erase your data, object to its
					processing or restrict it. The{" "}
					<Link to="/profile" className="text-accent hover:underline">
						profile page
					</Link>{" "}
					downloads a copy of everything the server holds for your account and
					deletes the account with its portfolios at once. For anything else,
					write to <ContactLink />; you will get an answer within a month.
				</p>
				<p>
					If you think your data is mishandled, you may also complain to the
					CNIL (cnil.fr), the French data protection authority.
				</p>
			</Section>

			<Section title="Security">
				<p>
					HTTPS everywhere, passwords stored only as bcrypt hashes, IP addresses
					hashed in the audit trail, no port of the server open to the internet,
					and a strict content security policy that forbids any third-party
					script.
				</p>
			</Section>
		</LegalPage>
	);
}
