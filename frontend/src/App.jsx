import { useState, useEffect } from 'react';
import Login from './components/Login';
import NoteList from './components/NoteList';
import NoteEditor from './components/NoteEditor';

function App() {
  const [token, setToken] = useState(() => localStorage.getItem('notenest_token') || '');
  const [userEmail, setUserEmail] = useState(() => localStorage.getItem('notenest_email') || '');
  const [notes, setNotes] = useState([]);
  const [activeNoteId, setActiveNoteId] = useState(null);
  const [searchQuery, setSearchQuery] = useState('');
  const [loading, setLoading] = useState(false);
  const [notifications, setNotifications] = useState([]);
  const [showNotifications, setShowNotifications] = useState(false);

  // Fetch notes whenever the token changes or is loaded
  useEffect(() => {
    if (token) {
      fetchNotes(token);
    } else {
      setNotes([]);
      setActiveNoteId(null);
      setNotifications([]);
    }
  }, [token]);

  // SSE via one-time ticket; EventSource cannot send headers.
  useEffect(() => {
    if (!token) return;

    let eventSource = null;
    let cancelled = false;

    const connect = async () => {
      try {
        const ticketRes = await fetch('/api/realtime/ticket', {
          method: 'POST',
          headers: { 'Authorization': `Bearer ${token}` },
        });
        if (!ticketRes.ok || cancelled) return;
        const { ticket } = await ticketRes.json();
        if (!ticket || cancelled) return;

        eventSource = new EventSource(`/api/events?ticket=${encodeURIComponent(ticket)}`);

        eventSource.onmessage = (event) => {
          try {
            const data = JSON.parse(event.data);
            if (data.type === 'note_shared') {
              const newNotif = {
                id: Date.now() + Math.random(),
                title: data.title || 'Shared Note',
                senderEmail: data.sender_email || 'A user',
                noteId: data.note_id,
                date: new Date(),
                read: false,
              };
              setNotifications((prev) => [newNotif, ...prev]);
              fetchNotes(token);
            }
          } catch (err) {
            console.error('Error parsing SSE event data:', err);
          }
        };

        eventSource.onerror = (err) => {
          console.warn('SSE connection error:', err);
        };
      } catch (err) {
        console.error('Failed to create EventSource connection:', err);
      }
    };

    connect();

    return () => {
      cancelled = true;
      if (eventSource) {
        eventSource.close();
      }
    };
  }, [token]);

  const fetchNotes = async (jwtToken) => {
    setLoading(true);
    try {
      const response = await fetch('/api/notes', {
        headers: {
          'Authorization': `Bearer ${jwtToken}`,
        },
      });

      if (response.status === 401) {
        handleLogout();
        return;
      }

      if (!response.ok) {
        throw new Error('Failed to fetch notes');
      }

      const data = await response.json();
      setNotes(data || []);
    } catch (err) {
      console.error('Error fetching notes:', err);
    } finally {
      setLoading(false);
    }
  };

  const handleLoginSuccess = (newToken, email) => {
    setToken(newToken);
    setUserEmail(email);
    localStorage.setItem('notenest_token', newToken);
    localStorage.setItem('notenest_email', email);
  };

  const handleLogout = () => {
    setToken('');
    setUserEmail('');
    localStorage.removeItem('notenest_token');
    localStorage.removeItem('notenest_email');
  };

  const handleCreateNote = async () => {
    try {
      const response = await fetch('/api/notes', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${token}`,
        },
        body: JSON.stringify({
          title: 'New Note',
          content: '',
        }),
      });

      if (response.status === 401) {
        handleLogout();
        return;
      }

      if (!response.ok) {
        throw new Error('Failed to create note');
      }

      const newNote = await response.json();
      setNotes((prev) => [newNote, ...prev]);
      setActiveNoteId(newNote.id);
    } catch (err) {
      console.error('Error creating note:', err);
    }
  };

  const handleSaveNote = async (id, title, content) => {
    try {
      const response = await fetch(`/api/notes/${id}`, {
        method: 'PUT',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${token}`,
        },
        body: JSON.stringify({ title, content }),
      });

      if (response.status === 401) {
        handleLogout();
        return;
      }

      if (!response.ok) {
        throw new Error('Failed to save note');
      }

      const updatedNote = await response.json();
      setNotes((prev) =>
        prev.map((note) => (note.id === id ? updatedNote : note))
      );
    } catch (err) {
      console.error('Error saving note:', err);
      throw err;
    }
  };

  const handleDeleteNote = async (id) => {
    try {
      const response = await fetch(`/api/notes/${id}`, {
        method: 'DELETE',
        headers: {
          'Authorization': `Bearer ${token}`,
        },
      });

      if (response.status === 401) {
        handleLogout();
        return;
      }

      if (!response.ok) {
        throw new Error('Failed to delete note');
      }

      setNotes((prev) => prev.filter((note) => note.id !== id));
      if (activeNoteId === id) {
        setActiveNoteId(null);
      }
    } catch (err) {
      console.error('Error deleting note:', err);
    }
  };

  const handleRefreshNote = async (id) => {
    try {
      const response = await fetch(`/api/notes/${id}`, {
        headers: {
          'Authorization': `Bearer ${token}`,
        },
      });

      if (response.status === 401) {
        handleLogout();
        return;
      }

      if (response.ok) {
        const refreshedNote = await response.json();
        setNotes((prev) =>
          prev.map((n) => (n.id === id ? refreshedNote : n))
        );
      }
    } catch (err) {
      console.error('Error refreshing note:', err);
    }
  };

  const handleShareNote = async (noteId, targetEmail, permission = 'editor') => {
    const response = await fetch(`/api/notes/${noteId}/share`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Authorization': `Bearer ${token}`,
      },
      body: JSON.stringify({ target_email: targetEmail, permission }),
    });

    if (response.status === 401) {
      handleLogout();
      return;
    }

    if (!response.ok) {
      const errData = await response.json().catch(() => ({}));
      throw new Error(errData.error || 'Failed to share note');
    }

    return await response.json();
  };

  const toggleNotifications = () => {
    setShowNotifications((prev) => !prev);
    setNotifications((prev) => prev.map((n) => ({ ...n, read: true })));
  };

  if (!token) {
    return <Login onLoginSuccess={handleLoginSuccess} />;
  }

  const activeNote = notes.find((n) => n.id === activeNoteId);
  const unreadCount = notifications.filter((n) => !n.read).length;

  return (
    <div className="app-container">
      <header className="app-header">
        <div className="logo-section">
          <span className="logo-icon">🪹</span>
          <span className="logo-text">NoteNest</span>
        </div>
        <div className="user-info">
          {loading && <span style={{ fontSize: '13px', color: 'var(--text-muted)' }}>Syncing...</span>}
          
          {/* Notifications Bell Icon Button & Dropdown */}
          <div className="notifications-bell-container">
            <button
              type="button"
              className="notifications-bell-btn"
              onClick={toggleNotifications}
              title="Notifications"
            >
              🔔
              {unreadCount > 0 && <span className="notification-badge">{unreadCount}</span>}
            </button>

            {showNotifications && (
              <div className="notifications-dropdown animate-fade-in">
                <div className="notifications-header">
                  <span>Notifications</span>
                  <button
                    type="button"
                    className="btn-icon"
                    onClick={() => setShowNotifications(false)}
                  >
                    ✕
                  </button>
                </div>
                <div className="notifications-list">
                  {notifications.length === 0 ? (
                    <div style={{ padding: '24px', textAlign: 'center', color: 'var(--text-muted)', fontSize: '13px' }}>
                      No notifications yet
                    </div>
                  ) : (
                    notifications.map((notif) => (
                      <div
                        key={notif.id}
                        className={`notification-item ${!notif.read ? 'unread' : ''}`}
                        onClick={() => {
                          if (notif.noteId) {
                            setActiveNoteId(notif.noteId);
                            setShowNotifications(false);
                          }
                        }}
                        style={{ cursor: notif.noteId ? 'pointer' : 'default' }}
                      >
                        <div className="notification-item-title">Shared Note Received</div>
                        <div className="notification-item-body">
                          <strong>{notif.senderEmail}</strong> shared note <em>"{notif.title}"</em> with you.
                        </div>
                        <div className="notification-item-time">
                          {new Date(notif.date).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}
                        </div>
                      </div>
                    ))
                  )}
                </div>
              </div>
            )}
          </div>
        </div>
      </header>

      <div className="workspace">
        <NoteList
          notes={notes}
          activeNoteId={activeNoteId}
          onSelectNote={setActiveNoteId}
          onDeleteNote={handleDeleteNote}
          onCreateNote={handleCreateNote}
          onLogout={handleLogout}
          userEmail={userEmail}
          searchQuery={searchQuery}
          setSearchQuery={setSearchQuery}
        />
        <NoteEditor
          note={activeNote}
          token={token}
          userEmail={userEmail}
          onSaveNote={handleSaveNote}
          onRefreshNote={handleRefreshNote}
          onShareNote={handleShareNote}
        />
      </div>
    </div>
  );
}

export default App;
