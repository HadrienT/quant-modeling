import { useState } from "react";
import { Download, Trash2 } from "lucide-react";
import { ApiError, fetchAccountExport } from "@/shared/api";
import { Button, Card, toast } from "@/shared/ui";
import { DeleteAccountDialog } from "./DeleteAccountDialog";

function download(name: string, blob: Blob) {
	const a = document.createElement("a");
	a.href = URL.createObjectURL(blob);
	a.download = name;
	a.click();
	URL.revokeObjectURL(a.href);
}

/** The GDPR rights the site can honour by itself: a copy, and erasure. */
export function AccountDataCard({ name }: { name: string }) {
	const [exporting, setExporting] = useState(false);
	const [deleting, setDeleting] = useState(false);

	const exportData = async () => {
		setExporting(true);
		try {
			const data = await fetchAccountExport();
			const day = data.exported_at.slice(0, 10);
			download(
				`quant-modeling-account-${day}.json`,
				new Blob([JSON.stringify(data, null, 2)], {
					type: "application/json",
				}),
			);
		} catch (e) {
			toast.error(
				e instanceof ApiError ? e.message : "The export could not be made",
			);
		} finally {
			setExporting(false);
		}
	};

	return (
		<Card className="flex flex-col gap-3">
			<h2 className="text-sm font-semibold text-ink">Your data</h2>
			<p className="text-xs text-ink-secondary">
				Download everything the server holds for this account (profile and
				portfolios) as JSON, or delete the account and its portfolios for good.
				What happens to the logs is in the privacy policy.
			</p>
			<div className="flex flex-wrap gap-2">
				<Button
					size="sm"
					variant="secondary"
					disabled={exporting}
					onClick={() => void exportData()}
				>
					<Download className="size-4" />
					{exporting ? "Preparing…" : "Download my data"}
				</Button>
				<Button size="sm" variant="ghost" onClick={() => setDeleting(true)}>
					<Trash2 className="size-4" />
					Delete account
				</Button>
			</div>
			<DeleteAccountDialog
				open={deleting}
				name={name}
				onOpenChange={setDeleting}
			/>
		</Card>
	);
}
