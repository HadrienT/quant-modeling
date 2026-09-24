import {
	type ComponentPropsWithoutRef,
	type ComponentRef,
	forwardRef,
} from "react";
import * as MenuPrimitive from "@radix-ui/react-dropdown-menu";
import { cn } from "./cn";

export const DropdownMenu = MenuPrimitive.Root;
export const DropdownMenuTrigger = MenuPrimitive.Trigger;
export const DropdownMenuGroup = MenuPrimitive.Group;

export const DropdownMenuContent = forwardRef<
	ComponentRef<typeof MenuPrimitive.Content>,
	ComponentPropsWithoutRef<typeof MenuPrimitive.Content>
>(({ className, align = "end", sideOffset = 6, ...props }, ref) => (
	<MenuPrimitive.Portal>
		<MenuPrimitive.Content
			ref={ref}
			align={align}
			sideOffset={sideOffset}
			collisionPadding={12}
			className={cn(
				"z-50 min-w-56 rounded-md border border-hairline bg-surface p-1 text-sm text-ink shadow-lg focus:outline-none",
				className,
			)}
			{...props}
		/>
	</MenuPrimitive.Portal>
));
DropdownMenuContent.displayName = "DropdownMenuContent";

export const DropdownMenuItem = forwardRef<
	ComponentRef<typeof MenuPrimitive.Item>,
	ComponentPropsWithoutRef<typeof MenuPrimitive.Item>
>(({ className, ...props }, ref) => (
	<MenuPrimitive.Item
		ref={ref}
		className={cn(
			"flex cursor-default items-center gap-2 rounded-sm px-2 py-1.5 text-sm text-ink-secondary transition-colors outline-none select-none data-[disabled]:pointer-events-none data-[disabled]:opacity-50 data-[highlighted]:bg-surface-raised data-[highlighted]:text-ink [&_svg]:size-4 [&_svg]:shrink-0",
			className,
		)}
		{...props}
	/>
));
DropdownMenuItem.displayName = "DropdownMenuItem";

export const DropdownMenuLabel = forwardRef<
	ComponentRef<typeof MenuPrimitive.Label>,
	ComponentPropsWithoutRef<typeof MenuPrimitive.Label>
>(({ className, ...props }, ref) => (
	<MenuPrimitive.Label
		ref={ref}
		className={cn("px-2 py-1.5 text-xs text-ink-secondary", className)}
		{...props}
	/>
));
DropdownMenuLabel.displayName = "DropdownMenuLabel";

export const DropdownMenuSeparator = forwardRef<
	ComponentRef<typeof MenuPrimitive.Separator>,
	ComponentPropsWithoutRef<typeof MenuPrimitive.Separator>
>(({ className, ...props }, ref) => (
	<MenuPrimitive.Separator
		ref={ref}
		className={cn("-mx-1 my-1 h-px bg-hairline", className)}
		{...props}
	/>
));
DropdownMenuSeparator.displayName = "DropdownMenuSeparator";
