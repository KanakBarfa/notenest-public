export default function NoteList({ 
  notes, 
  activeNoteId, 
  onSelectNote, 
  onDeleteNote, 
  onCreateNote, 
  onLogout, 
  userEmail,
  searchQuery,
  setSearchQuery 
}) {
  
  const filteredNotes = notes.filter(note => {
    const query = searchQuery.toLowerCase();
    return (
      note.title.toLowerCase().includes(query) || 
      note.content.toLowerCase().includes(query)
    );
  });

  const formatDate = (dateStr) => {
    if (!dateStr) return '';
    try {
      const date = new Date(dateStr);
      // Format to readable locale format
      return date.toLocaleDateString(undefined, { 
        month: 'short', 
        day: 'numeric', 
        hour: '2-digit', 
        minute: '2-digit' 
      });
    } catch {
      return dateStr;
    }
  };

  return (
    <aside className="sidebar">
      <div className="sidebar-actions">
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <span style={{ fontSize: '12px', color: 'var(--text-muted)' }}>Logged in as:</span>
          <button 
            type="button" 
            className="btn btn-secondary" 
            style={{ padding: '4px 8px', fontSize: '12px' }}
            onClick={onLogout}
          >
            Log Out
          </button>
        </div>
        <div style={{ fontSize: '13px', fontWeight: 600, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          {userEmail}
        </div>
        
        <button 
          type="button" 
          className="btn btn-primary" 
          onClick={onCreateNote}
          style={{ width: '100%' }}
        >
          + New Note
        </button>

        <input
          type="text"
          placeholder="Search notes..."
          className="search-input"
          value={searchQuery}
          onChange={(e) => setSearchQuery(e.target.value)}
        />
      </div>

      <div className="notes-list">
        {filteredNotes.length === 0 ? (
          <div className="no-notes">
            {notes.length === 0 ? 'No notes yet. Create one!' : 'No matching notes found.'}
          </div>
        ) : (
          filteredNotes.map(note => (
            <div 
              key={note.id} 
              className={`note-card ${activeNoteId === note.id ? 'active' : ''}`}
              onClick={() => onSelectNote(note.id)}
            >
              <div className="note-card-title">{note.title || 'Untitled Note'}</div>
              <div className="note-card-excerpt">{note.content || 'No content...'}</div>
              <div className="note-card-footer">
                <span className="note-card-date">{formatDate(note.created_at)}</span>
                <button 
                  type="button" 
                  className="note-card-delete"
                  onClick={(e) => {
                    e.stopPropagation();
                    if (confirm('Are you sure you want to delete this note?')) {
                      onDeleteNote(note.id);
                    }
                  }}
                  title="Delete Note"
                >
                  🗑️
                </button>
              </div>
            </div>
          ))
        )}
      </div>
    </aside>
  );
}
