/* SPDX-License-Identifier: GPL-2.0 */
/* Force alignment of .toc section.  */
SECTIONS
{
	.toc 0 : ALIGN(256)
	{
		*(.got .toc)
	}
}
