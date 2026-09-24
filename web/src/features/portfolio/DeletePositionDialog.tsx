import type { PositionMark } from "@/shared/api";
import {
	Button,
	Dialog,
	DialogContent,
	DialogDescription,
	DialogHeader,
	DialogTitle,
} from "@/shared/ui";

/**
 * Confirms deleting a position booked by mistake: its trades leave the
 * journal as if never booked, and the P&L history is recomputed without
 * them. A real position is closed by a sale, which keeps its P&L.
 */
export function DeletePositionDialog({
	position,
	trades,
	onConfirm,
	onCancel,
}: {
	position: PositionMark | null;
	trades: number;
	onConfirm: () => void;
	onCancel: () => void;
}) {
	return (
		<Dialog open={!!position} onOpenChange={(open) => !open && onCancel()}>
			<DialogContent>
				<DialogHeader>
					<DialogTitle>Delete {position?.label}?</DialogTitle>
					<DialogDescription>
						Its {trades} trade{trades === 1 ? "" : "s"} leave the journal as if
						never booked, and the P&L history is recomputed without them. Use
						this for a position entered by mistake — to close a real position,
						sell it instead, which keeps its realised P&L.
					</DialogDescription>
				</DialogHeader>
				<div className="mt-4 flex justify-end gap-2">
					<Button size="sm" variant="ghost" onClick={onCancel}>
						Keep it
					</Button>
					<Button size="sm" variant="danger" onClick={onConfirm}>
						Delete position
					</Button>
				</div>
			</DialogContent>
		</Dialog>
	);
}
