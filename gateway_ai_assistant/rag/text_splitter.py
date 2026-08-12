from __future__ import annotations


def split_text(text: str, chunk_size: int = 500, overlap: int = 50) -> list[str]:
    text = text.strip()
    if not text:
        return []
    chunks = []
    start = 0
    while start < len(text):
        end = min(len(text), start + chunk_size)
        chunks.append(text[start:end])
        if end >= len(text):
            break
        start = max(end - overlap, start + 1)
    return chunks


def split_text_with_offsets(text: str, chunk_size: int = 500, overlap: int = 50) -> list[tuple[str, int, int]]:
    text = text.strip()
    if not text:
        return []
    chunks: list[tuple[str, int, int]] = []
    start = 0
    while start < len(text):
        end = min(len(text), start + chunk_size)
        chunks.append((text[start:end], start, end))
        if end >= len(text):
            break
        start = max(end - overlap, start + 1)
    return chunks


def split_markdown_sections(text: str, chunk_size: int = 500, overlap: int = 50) -> list[dict[str, object]]:
    lines = [line.rstrip() for line in text.splitlines()]
    sections: list[dict[str, str]] = []
    current_title = "文档开头"
    buf: list[str] = []

    for line in lines:
        stripped = line.strip()
        if stripped.startswith("#"):
            if buf:
                sections.append({"section_title": current_title, "content": "\n".join(buf).strip()})
                buf = []
            current_title = stripped.lstrip("#").strip() or "未命名章节"
            buf.append(line)
            continue
        buf.append(line)

    if buf:
        sections.append({"section_title": current_title, "content": "\n".join(buf).strip()})

    chunk_items: list[dict[str, object]] = []
    chunk_seq = 0
    for section_index, section in enumerate(sections):
        section_title = str(section["section_title"])
        content = str(section["content"]).strip()
        if not content:
            continue
        for chunk_text, start_offset, end_offset in split_text_with_offsets(
            content,
            chunk_size=chunk_size,
            overlap=overlap,
        ):
            chunk_items.append(
                {
                    "chunk_text": chunk_text,
                    "section_title": section_title,
                    "section_index": section_index,
                    "chunk_index_in_section": chunk_seq,
                    "start_offset": start_offset,
                    "end_offset": end_offset,
                }
            )
            chunk_seq += 1
    return chunk_items


def split_markdown_text(text: str, chunk_size: int = 500, overlap: int = 50) -> list[str]:
    return [
        str(item["chunk_text"])
        for item in split_markdown_sections(text, chunk_size=chunk_size, overlap=overlap)
        if str(item["chunk_text"]).strip()
    ]
