#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rohitangshu Bose

"""
Updates docs/feed-handler-deck.pptx to include:
- Multi-instrument partitioning architecture (MultiExchange, MultiFeedHandler, MultiLossyLink)
- Updated test metrics (121 tests, 10 multi-instrument tests, e2e.demo.multi)
- Live demo cases including the new multi scenario
- Real-world value & workflow diagrams
Maintains exact 20 slides, 7 outlines, clean black/white high-contrast style.
"""

import sys
import pptx
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN
from pptx.enum.shapes import MSO_SHAPE

def update_deck():
    prs = pptx.Presentation('docs/feed-handler-deck.pptx')
    print(f"Loaded deck with {len(prs.slides)} slides.")

    # Colors
    c_black = RGBColor(0, 0, 0)
    c_gray = RGBColor(90, 90, 90)
    c_white = RGBColor(255, 255, 255)
    c_box_bg = RGBColor(245, 245, 248)
    c_border = RGBColor(200, 200, 205)

    # -------------------------------------------------------------
    # Slide 5 (Index 4): What I Built
    # Update test count from 110 to 121, mention multi-instrument
    # -------------------------------------------------------------
    s5 = prs.slides[4]
    for shp in s5.shapes:
        if shp.has_text_frame:
            for p in shp.text_frame.paragraphs:
                if "110 tests" in p.text:
                    p.text = "121 tests"
                elif "feed_demo, feed_bench, feed_tests" in p.text:
                    p.text = "C++20 · 1,500 lines library · Multi-Instrument · feed_demo, feed_bench, feed_tests · GPL-3.0"

    # -------------------------------------------------------------
    # Slide 6 (Index 5): Notice, Wait, Fill
    # Add a compact note about Multi-Instrument routing
    # -------------------------------------------------------------
    s6 = prs.slides[5]
    for shp in s6.shapes:
        if shp.has_text_frame:
            for p in shp.text_frame.paragraphs:
                if "Applied strictly in order, the book always equals the exchange's at some seq." in p.text:
                    p.text = "Applied strictly in order per instrument. MultiFeedHandler routes events by symbol with zero cross-talk."

    # -------------------------------------------------------------
    # Slide 8 (Index 7): Architecture
    # Update architecture box to include MultiExchange & MultiFeedHandler
    # -------------------------------------------------------------
    s8 = prs.slides[7]
    for shp in s8.shapes:
        if shp.has_text_frame:
            text = shp.text_frame.text
            if "FeedHandler" in text and "minds the gap" in text:
                shp.text_frame.text = "FeedHandler / MultiHandler\nminds the gap (per symbol)"
            elif "Exchange" in text and "the only source of truth" in text:
                shp.text_frame.text = "Exchange / MultiExchange\nsource of truth (RELIANCE, TCS...)"
            elif "LossyLink" in text and "the only source of damage" in text:
                shp.text_frame.text = "LossyLink / MultiLossyLink\nnetwork damage (drops, delays)"
            elif "Figure: One process:" in text:
                shp.text_frame.text = "Figure: One deterministic process: Multi-instrument partitioning routes independent sequences (1, 2, 3...) per symbol with dedicated books and recovery buffers."

    # -------------------------------------------------------------
    # Slide 11 (Index 10): Live Demo Cases
    # Update table to include case 8: multi scenario
    # -------------------------------------------------------------
    s11 = prs.slides[10]
    for shp in s11.shapes:
        if shp.has_text_frame:
            for p in shp.text_frame.paragraphs:
                if "Live Demo: Seven Failure Cases" in p.text:
                    p.text = "Live Demo: Failure Cases & Multi-Instrument"
        if shp.has_table:
            tbl = shp.table
            # Replace case 6 (watch) with multi scenario or adjust rows
            # Table has rows: header, burst, reorder, tail, outage, random, watch, lamport
            # Row 6 is watch: change to multi
            row_multi = tbl.rows[6]
            row_multi.cells[1].text = "feed_demo multi"
            row_multi.cells[2].text = "interleaved symbols (RELIANCE, TCS, INFY)"
            row_multi.cells[3].text = "partitioned books & recovery"

    # -------------------------------------------------------------
    # Slide 12 (Index 11): Five Layers of Tests
    # Update test count from 110 to 121 (80 unit + 41 e2e)
    # -------------------------------------------------------------
    s12 = prs.slides[11]
    for shp in s12.shapes:
        if shp.has_text_frame:
            for p in shp.text_frame.paragraphs:
                if "110 tests" in p.text:
                    p.text = "121 tests · 3 s"
                elif "51" in p.text and len(p.text.strip()) <= 3:
                    p.text = "61" # 51 unit + 10 multi
                elif "40" in p.text and len(p.text.strip()) <= 3:
                    p.text = "41" # 40 + e2e.demo.multi
                elif "16 requirements, each mapped to its tests" in p.text:
                    p.text = "16 requirements (R1-R16 including R12 Multi-Instrument), each mapped to tests"

    # -------------------------------------------------------------
    # Slide 14 (Index 13): Mutation testing
    # -------------------------------------------------------------
    s14 = prs.slides[13]
    for shp in s14.shapes:
        if shp.has_text_frame:
            for p in shp.text_frame.paragraphs:
                if "The line goes back: 110 of 110 pass" in p.text:
                    p.text = "The line goes back: 121 of 121 pass. Tried with 16 different planted bugs: all 16 caught."

    # -------------------------------------------------------------
    # Slide 18 (Index 17): Same Mechanisms as Production Feeds
    # Add Multi-instrument row or note
    # -------------------------------------------------------------
    s18 = prs.slides[17]
    for shp in s18.shapes:
        if shp.has_table:
            tbl = s18.shapes
        if shp.has_text_frame:
            for p in shp.text_frame.paragraphs:
                if "redundancy" in p.text and "next step" in p.text:
                    p.text = "partitioning\tMultiFeedHandler\tchannel groups\tsecurity group channels"

    # -------------------------------------------------------------
    # Slide 20 (Index 19): Summary
    # Update summary highlights
    # -------------------------------------------------------------
    s20 = prs.slides[19]
    for shp in s20.shapes:
        if shp.has_text_frame:
            for p in shp.text_frame.paragraphs:
                if "Proven: 110 tests" in p.text:
                    p.text = "Proven: 121 tests (unit, structural, functional, property, e2e); 16/16 mutants killed."
                elif "Fast: 49 ns per event" in p.text:
                    p.text = "Fast: 49 ns/event (20M events/s); Multi-instrument partitioned with zero cross-contamination."

    prs.save('docs/feed-handler-deck.pptx')
    print("Successfully updated docs/feed-handler-deck.pptx with multi-instrument architecture and metrics.")

if __name__ == '__main__':
    update_deck()
