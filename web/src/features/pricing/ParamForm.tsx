import { useEffect } from "react";
import { useForm } from "react-hook-form";
import { zodResolver } from "@hookform/resolvers/zod";
import type { ProductDescriptor } from "@/shared/products";
import { Input, Label } from "@/shared/ui";
import { fieldsFromSchema } from "./zodFields";

/**
 * Form derived entirely from the product's zod schema (WP 07 §2). Only the
 * selected product's fields exist; changing product remounts this component via
 * a `key`, so there is never residual state.
 */
export function ParamForm({
	descriptor,
	values,
	onChange,
}: {
	descriptor: ProductDescriptor;
	values: Record<string, unknown>;
	onChange: (v: Record<string, unknown>) => void;
}) {
	const fields = fieldsFromSchema(descriptor.schema);
	const {
		register,
		watch,
		formState: { errors },
	} = useForm({
		resolver: zodResolver(descriptor.schema as never),
		defaultValues: values as never,
		mode: "onChange",
	});

	// push valid form state up (debounced by the browser's input cadence)
	useEffect(() => {
		const sub = watch((v) => onChange(v as Record<string, unknown>));
		return () => sub.unsubscribe();
	}, [watch, onChange]);

	const groups = ["contract", "market", "engine"] as const;

	return (
		<form className="flex flex-col gap-4">
			{groups.map((g) => {
				const fs = fields.filter((f) => f.group === g);
				if (!fs.length) return null;
				return (
					<fieldset key={g} className="flex flex-col gap-2">
						<legend className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
							{g}
						</legend>
						<div className="grid grid-cols-2 gap-2">
							{fs.map((f) => {
								const err = (errors as Record<string, { message?: string }>)[
									f.name
								]?.message;
								return (
									<div key={f.name} className="flex flex-col gap-1">
										<Label htmlFor={`f-${f.name}`}>
											{f.label}
											{f.unit ? ` (${f.unit})` : ""}
										</Label>
										{f.kind === "boolean" ? (
											<label className="flex h-9 items-center gap-2 text-sm text-ink">
												<input
													id={`f-${f.name}`}
													type="checkbox"
													{...register(f.name as never)}
												/>
												{f.label}
											</label>
										) : f.kind === "enum" ? (
											<select
												id={`f-${f.name}`}
												className="h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
												{...register(f.name as never)}
											>
												{f.options!.map((o) => (
													<option key={o} value={o}>
														{o}
													</option>
												))}
											</select>
										) : (
											<Input
												id={`f-${f.name}`}
												type={f.kind === "number" ? "number" : "text"}
												step={f.step}
												{...register(f.name as never)}
											/>
										)}
										{err && (
											<span className="text-2xs text-critical">{err}</span>
										)}
									</div>
								);
							})}
						</div>
					</fieldset>
				);
			})}
		</form>
	);
}
