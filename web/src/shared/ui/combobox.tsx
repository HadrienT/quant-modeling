import { useState } from "react";
import { Command } from "cmdk";
import { Check, ChevronsUpDown } from "lucide-react";
import { Popover, PopoverContent, PopoverTrigger } from "./popover";
import { cn } from "./cn";

/** Searchable single-select (WP 08). shadcn combobox = command + popover. */
export function Combobox({
	options,
	value,
	onChange,
	placeholder = "Select…",
	className,
}: {
	options: { value: string; label?: string }[];
	value: string | null;
	onChange: (v: string) => void;
	placeholder?: string;
	className?: string;
}) {
	const [open, setOpen] = useState(false);
	const current = options.find((o) => o.value === value);

	return (
		<Popover open={open} onOpenChange={setOpen}>
			<PopoverTrigger asChild>
				<button
					type="button"
					aria-haspopup="listbox"
					aria-expanded={open}
					className={cn(
						"flex h-9 w-full items-center justify-between rounded-sm border border-hairline bg-surface px-3 text-sm text-ink",
						className,
					)}
				>
					{current?.label ?? current?.value ?? placeholder}
					<ChevronsUpDown className="size-3.5 text-ink-muted" />
				</button>
			</PopoverTrigger>
			<PopoverContent
				align="start"
				className="w-[--radix-popover-trigger-width] p-0"
			>
				<Command>
					<Command.Input
						placeholder="Search…"
						className="w-full border-b border-hairline bg-transparent px-3 py-2 text-sm text-ink outline-none placeholder:text-ink-muted"
					/>
					<Command.List className="max-h-64 overflow-y-auto p-1">
						<Command.Empty className="px-3 py-4 text-center text-xs text-ink-muted">
							No match.
						</Command.Empty>
						{options.map((o) => (
							<Command.Item
								key={o.value}
								value={o.value}
								onSelect={() => {
									onChange(o.value);
									setOpen(false);
								}}
								className="flex cursor-pointer items-center gap-2 rounded-sm px-2 py-1.5 text-sm text-ink-secondary data-[selected=true]:bg-surface-raised data-[selected=true]:text-ink"
							>
								<Check
									className={cn(
										"size-3.5",
										value === o.value ? "opacity-100" : "opacity-0",
									)}
								/>
								{o.label ?? o.value}
							</Command.Item>
						))}
					</Command.List>
				</Command>
			</PopoverContent>
		</Popover>
	);
}
