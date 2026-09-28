/**
 * What the site processes, why, on what legal basis (GDPR art. 6) and for how
 * long. Each duration is the one the deployment enforces, not a promise: the
 * user store and portfolios until deletion, the API log file rotated after 30
 * days (`logging_utils.py`), Loki 720 h, Tempo 72 h, the audit database's
 * monthly partitions after 12 months (quant-platform `AUDIT_RETENTION_MONTHS`).
 */
const ROWS: { what: string; why: string; basis: string; kept: string }[] = [
	{
		what: "Account: your username and a bcrypt hash of your password, or, with Google, your Google account identifier and email address; the creation date",
		why: "Sign you in",
		basis: "Contract (the service you ask for)",
		kept: "Until you delete the account",
	},
	{
		what: "Portfolios saved to your account",
		why: "Keep them across devices",
		basis: "Contract",
		kept: "Until you delete them or the account",
	},
	{
		what: "Messages to the scripting assistant",
		why: "Write the script you ask for",
		basis: "Contract",
		kept: "Not stored: the model runs on the same server and forgets the conversation; only its duration and whether the script was valid are logged",
	},
	{
		what: "Web server logs: IP address, time, page or API route, status, browser user agent",
		why: "Operate the site, investigate faults and attacks",
		basis: "Legitimate interest",
		kept: "30 days",
	},
	{
		what: "Request traces: timing of each step of a request, which may include the IP address and username",
		why: "Diagnose slow or failed requests",
		basis: "Legitimate interest",
		kept: "3 days",
	},
	{
		what: "Audit trail: sign-ins (successful or not), account creation and deletion, and each valuation's inputs and result, with your username and a keyed hash of your IP address (never the address itself)",
		why: "Detect account attacks; reproduce a price exactly as it was computed",
		basis: "Legitimate interest",
		kept: "12 months, then deleted with the month they belong to. This log cannot be edited, so deleting your account does not remove its earlier entries: they expire on this schedule",
	},
];

export function ProcessingTable() {
	return (
		<div className="-mx-4 overflow-x-auto px-4">
			<table className="w-full min-w-[36rem] border-collapse text-left text-xs">
				<thead className="text-ink">
					<tr className="border-b border-hairline">
						<th className="py-2 pr-3 font-semibold">Data</th>
						<th className="py-2 pr-3 font-semibold">Purpose</th>
						<th className="py-2 pr-3 font-semibold">Legal basis</th>
						<th className="py-2 font-semibold">Kept</th>
					</tr>
				</thead>
				<tbody>
					{ROWS.map((r) => (
						<tr key={r.what} className="border-b border-hairline align-top">
							<td className="py-2 pr-3">{r.what}</td>
							<td className="py-2 pr-3">{r.why}</td>
							<td className="py-2 pr-3">{r.basis}</td>
							<td className="py-2">{r.kept}</td>
						</tr>
					))}
				</tbody>
			</table>
		</div>
	);
}
