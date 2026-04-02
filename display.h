#ifndef DISPLAY_H
#define DISPLAY_H

// Called from loop(). Handles deferred full-quality refresh after rapid navigation.
void displayLoop();

// Render and display a stored page. Handles refresh strategy (partial vs full).
// forceFullRefresh: true for document changes, false for same-doc page changes.
void displayShowPage(int docId, int pageNum,
                     int docIndex, int docCount, int pageCount,
                     bool forceFullRefresh);

// Show the "no documents" idle screen.
void displayShowEmpty();

// Show delete confirmation dialog.
void displayShowDeleteConfirm(const char* docName);

// Set a notification message shown on the next render, or NULL to clear.
void displaySetNotification(const char* msg);

#endif // DISPLAY_H
